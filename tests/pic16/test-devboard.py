#!/usr/bin/env python3
"""End-to-end check of the PIC16 emulation, with no product in it.

Runs tests/pic16/fw_devboard.asm on -M pic16-devboard against the reference
model in bridge_model.py, and checks what the firmware reports over the
console. Between them these cover every route the guest has to the outside
world:

  console output        the EUSART, through PPS-routed pins
  edges out             a port pin reaching the model over the bridge
  a level in            the model driving a port pin the firmware reads
  an SPI read           the model driving a chip QEMU emulates
  a power cycle         the guest resetting the board, model included
  a watchdog reset      the same power cycle, driven by a timer instead

Usage: test-devboard.py <qemu-system-pic16>
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# Generous because the firmware spends most of it starving the watchdog: the
# period is 2.1 s of virtual time, and under -icount that is a fixed number of
# instructions rather than a fixed wall-clock wait.
RUN_SECONDS = 60

# What the model drives onto the expander's pins, and so what the firmware must
# read back over SPI. An arbitrary pattern, chosen to catch bit ordering.
EXPANDER_PATTERN = 0xA5


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    qemu = argv[1]
    failures = []

    def check(name, got, want):
        ok = got == want
        print(f'{"PASS" if ok else "FAIL"} {name}')
        if not ok:
            print(f'     wanted {want!r}\n     got    {got!r}')
            failures.append(name)

    with tempfile.TemporaryDirectory(prefix='pic16-devboard-') as work:
        hexfile = os.path.join(work, 'fw.hex')
        proc = subprocess.run(
            ['gpasm', '-p', '16f1829', '-I', HERE, '-o', hexfile,
             os.path.join(HERE, 'fw_devboard.asm')],
            capture_output=True, text=True)
        noise = [l for l in proc.stderr.splitlines()
                 if l.strip() and 'Processor superseded' not in l]
        if proc.returncode != 0:
            print('FAIL assembly\n' + '\n'.join(noise) or proc.stdout)
            return 1

        sock = os.path.join(work, 'rig.sock')
        status = os.path.join(work, 'status.json')
        console = os.path.join(work, 'console.txt')

        drive = [f'expander.GP{bit}={(EXPANDER_PATTERN >> bit) & 1}'
                 for bit in range(8)]
        model = subprocess.Popen(
            [sys.executable, os.path.join(HERE, 'bridge_model.py'),
             '--listen', sock, '--status', status,
             '--watch', 'soc.RA5=rising',
             '--drive', 'soc.RB5=1', *sum(([f'--drive', d] for d in drive), [])],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        deadline = time.time() + 5
        while not os.path.exists(sock):
            if time.time() > deadline:
                print('FAIL the model never created its socket')
                model.kill()
                return 1
            time.sleep(0.05)

        qemu_proc = subprocess.Popen(
            [qemu, '-M', 'pic16-devboard', '-bios', hexfile,
             '-display', 'none', '-nodefaults', '-icount', 'shift=3',
             '-serial', f'file:{console}',
             '-chardev', f'socket,id=rig,path={sock}', '-serial', 'chardev:rig'],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

        # The firmware ends in a spin loop, so wait for its last line rather
        # than for it to exit.
        deadline = time.time() + RUN_SECONDS
        text = ''
        while time.time() < deadline:
            if os.path.exists(console):
                text = open(console, errors='replace').read()
                if 'DONE' in text:
                    break
            time.sleep(0.2)

        qemu_proc.kill()
        qemu_proc.wait()
        model.wait(timeout=5)

        print(f'--- console ---\n{text.strip()}\n---')

        check('banner', 'PIC16 OK' in text, True)
        check('level driven in', re.search(r'B5=(\d)', text).group(1)
              if re.search(r'B5=(\d)', text) else None, '1')
        check('expander read over SPI',
              re.search(r'GP=([0-9A-F]{2})', text).group(1)
              if re.search(r'GP=([0-9A-F]{2})', text) else None,
              f'{EXPANDER_PATTERN:02X}')
        check('firmware ran to completion', 'DONE' in text, True)

        # Each pass configures RC1PPS and resets; the last reports what it
        # found there before configuring anything. Zero means the whole board
        # came back, not just the core -- which is the difference between a
        # power cycle and a jump to the reset vector.
        check('the reset power-cycled the peripherals',
              re.search(r'PPS=([0-9A-F]{2})', text).group(1)
              if re.search(r'PPS=([0-9A-F]{2})', text) else None, '00')

        # PCON0's RWDT reads 0 when the watchdog caused the last reset, so
        # this says the third pass is the far side of a watchdog power cycle
        # and not of the RESET instruction that started the second.
        check('the watchdog reset the board',
              re.search(r'WDT=(\d)', text).group(1)
              if re.search(r'WDT=(\d)', text) else None, '0')

        report = {}
        if os.path.exists(status):
            report = json.load(open(status))
        check('edges out', report.get('edges', {}).get('soc.RA5'), 3)
        # One RESET event for each power cycle, and the model outside QEMU
        # hears about both: the instruction's and the watchdog's.
        check('the model was told about both resets', report.get('resets'), 2)

    print('\n' + ('FAILED: ' + ', '.join(failures) if failures
                  else 'all checks passed'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
