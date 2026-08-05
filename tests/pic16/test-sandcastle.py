#!/usr/bin/env python3
"""End-to-end check of the sandcastle machine against a released firmware image.

QEMU models only the chips. The mechanism lives in the product repository and
is reached over the simulation bridge (target/pic16/SIM-BRIDGE.md), so this
starts that model as a subprocess and lets QEMU talk to it.

Homing is the interesting case: it completes only if step pulses reach the
external model, the inner limit switch it drives comes back through a SoC pin,
and the theta index sensor comes back through the SPI expander.

Usage:
  test-sandcastle.py <qemu-system-pic16> <firmware.hex> <sandcastle_bridge>

The bridge binary is built in the product repository with `make bridge`.
"""

import os
import socket
import subprocess
import sys
import tempfile
import time

BOOT_WAIT = 5.0
PORT = 45899

# Start just off the inner switch and a half turn from the index tape, so
# homing has to seek for both rather than beginning on them.
MODEL_ENV = {
    'SANDCASTLE_SIM_START_R_MM': '16.3',
    'SANDCASTLE_SIM_START_THETA_DEG': '180',
}


class Rig:
    """The external physical model, plus the QEMU it is wired to."""

    def __init__(self, qemu, firmware, bridge):
        self.dir = tempfile.mkdtemp(prefix='pic16-rig-')
        self.socket_path = os.path.join(self.dir, 'rig.sock')

        env = dict(os.environ, **MODEL_ENV)
        self.model = subprocess.Popen([bridge, '--listen', self.socket_path],
                                      env=env, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL)
        self._await_socket()

        self.qemu = subprocess.Popen(
            [qemu, '-M', 'sandcastle', '-bios', firmware,
             '-display', 'none', '-nodefaults', '-icount', 'shift=3',
             '-chardev',
             f'socket,id=console,host=127.0.0.1,port={PORT},'
             'server=on,wait=off',
             '-serial', 'chardev:console',
             '-chardev', f'socket,id=rig,path={self.socket_path}',
             '-serial', 'chardev:rig'],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

        deadline = time.time() + BOOT_WAIT
        while True:
            try:
                self.console = socket.create_connection(('127.0.0.1', PORT), 1)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.console.settimeout(2)

    def _await_socket(self):
        deadline = time.time() + BOOT_WAIT
        while not os.path.exists(self.socket_path):
            if time.time() > deadline:
                raise RuntimeError('the model never created its socket')
            time.sleep(0.05)

    def command(self, text, timeout=240.0):
        try:
            while self.console.recv(4096):
                pass
        except socket.timeout:
            pass

        self.console.sendall(text.encode())
        buf, deadline = b'', time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = self.console.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            buf += chunk
            if b'ok\r\n' in buf:
                break
        return buf.decode(errors='replace')

    def close(self):
        for proc in (self.qemu, self.model):
            proc.kill()
            proc.wait()


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2

    failures = []

    def check(name, got, want):
        ok = want in got
        print(f'{"PASS" if ok else "FAIL"} {name}')
        if not ok:
            print(f'     wanted {want!r}\n     got    {got!r}')
            failures.append(name)

    rig = Rig(argv[1], argv[2], argv[3])
    try:
        check('version', rig.command('M115\n', 20),
              'FIRMWARE_NAME:Sandcastle')

        # The model starts the carriage off both switches and away from the
        # index tape, so nothing should be asserted yet.
        check('switches idle', rig.command('M114\n', 20),
              'LIMIT1:0 LIMIT2:0 THETA_INDEX:0 HOMED:0')

        # Homing seeks the inner switch, backs off, approaches slowly, then
        # rotates until the index tape appears. The tape arrives through the
        # SPI expander, so this covers both return paths.
        check('homing', rig.command('G28\n'), 'ok')
        check('homed at the switch', rig.command('M114\n', 20),
              'R:15.88 T:0.00 LIMIT1:1 LIMIT2:0 THETA_INDEX:1 HOMED:1')

        # A move must leave the switch behind and land where it was asked to.
        rig.command('G1 X40 Y0 F600\n')
        check('moved', rig.command('M114\n', 20),
              'X:40.00 Y:0.00 R:40.00 T:0.00 LIMIT1:0')
    finally:
        rig.close()

    print('\n' + ('FAILED: ' + ', '.join(failures) if failures
                  else 'all checks passed'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
