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
  a second one          two of a kind, on different chip selects
  an LED frame          a bit-banged WS2812 strip, decoded to colours
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

# The board, as the machine is told about it: two expanders on separate chip
# selects and a four-pixel strip. Nothing is fitted by default -- the point of
# a development board is that what is on it is not compiled in.
EXPANDERS = {'RC7': 'RA2', 'RC3': None}
LED_PIN = 'RB7'
LED_PIXELS = 4

# What the model drives onto each expander's pins, and so what the firmware
# must read back over SPI. Arbitrary patterns, chosen to catch bit ordering
# and to be different from each other.
EXPANDER_PATTERN = {'RC7': 0xA5, 'RC3': 0x3C}

# What fw_devboard.asm shifts out of the strip: two red, one blue, one green.
LED_FRAME = '2xFF0000 1x0000FF 1x00FF00'


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

        drive = ['soc.RB5=1']
        for cs, pattern in EXPANDER_PATTERN.items():
            drive += [f'expander.{cs}.GP{bit}={(pattern >> bit) & 1}'
                      for bit in range(8)]
        model = subprocess.Popen(
            [sys.executable, os.path.join(HERE, 'bridge_model.py'),
             '--listen', sock, '--status', status,
             '--watch', 'soc.RA5=rising',
             *sum((['--drive', d] for d in drive), [])],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        deadline = time.time() + 5
        while not os.path.exists(sock):
            if time.time() > deadline:
                print('FAIL the model never created its socket')
                model.kill()
                return 1
            time.sleep(0.05)

        expanders = '/'.join(cs + (f':{i}' if i else '')
                             for cs, i in EXPANDERS.items())
        machine = (f'pic16-devboard,expanders={expanders},'
                   f'leds={LED_PIN}:{LED_PIXELS}')
        qemu_proc = subprocess.Popen(
            [qemu, '-M', machine, '-bios', hexfile,
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
        # Two expanders, two patterns. Getting the same byte twice would mean
        # one chip select is doing both chips' work, which is exactly the
        # mistake that hides when a board has only one of something.
        for label, cs in (('GP', 'RC7'), ('GQ', 'RC3')):
            found = re.search(label + r'=([0-9A-F]{2})', text)
            check(f'expander on {cs} read over SPI',
                  found.group(1) if found else None,
                  f'{EXPANDER_PATTERN[cs]:02X}')
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

        # The strip is a wire carrying a self-clocked bit stream. QEMU decodes
        # it and reports colours, so nothing outside has to know the timing --
        # and getting the run lengths and the GRB order right is the proof.
        check('the LED frame reached the model as colours',
              report.get('leds', {}).get(f'led.{LED_PIN}'), LED_FRAME)
        # One RESET event for each power cycle, and the model outside QEMU
        # hears about both: the instruction's and the watchdog's.
        check('the model was told about both resets', report.get('resets'), 2)

    print('\n' + ('FAILED: ' + ', '.join(failures) if failures
                  else 'all checks passed'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
