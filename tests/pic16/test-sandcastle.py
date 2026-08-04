#!/usr/bin/env python3
"""End-to-end check of the sandcastle machine against a released firmware image.

Boots the image, then drives the G-code console over a socket chardev. Homing
is the interesting case: it only completes if step pulses reach the mechanics
model and the switches it drives are read back through both the port and the
SPI expander.

Usage: test-sandcastle.py <qemu-system-pic16> <firmware.hex>
"""

import socket
import subprocess
import sys
import time

BOOT_WAIT = 3.0
PORT = 45899


class Console:
    def __init__(self, qemu, firmware, port):
        self.proc = subprocess.Popen(
            [qemu, '-M', 'sandcastle', '-bios', firmware,
             '-display', 'none', '-monitor', 'none', '-nodefaults',
             '-icount', 'shift=3',
             '-chardev',
             f'socket,id=s0,host=127.0.0.1,port={port},server=on,wait=off',
             '-serial', 'chardev:s0'],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        deadline = time.time() + BOOT_WAIT
        while True:
            try:
                self.sock = socket.create_connection(('127.0.0.1', port), 1)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.sock.settimeout(2)

    def command(self, text, timeout=60.0):
        self.sock.sendall(text.encode())
        buf, deadline = b'', time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            buf += chunk
            if b'ok\r\n' in buf:
                break
        return buf.decode(errors='replace')

    def close(self):
        self.proc.kill()
        self.proc.wait()


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2

    failures = []

    def check(name, got, want):
        ok = want in got
        print(f'{"PASS" if ok else "FAIL"} {name}')
        if not ok:
            print(f'     wanted {want!r}\n     got    {got!r}')
            failures.append(name)

    console = Console(argv[1], argv[2], PORT)
    try:
        check('version', console.command('M115\n', 10),
              'FIRMWARE_NAME:Sandcastle')

        # Before homing the carriage sits off both switches.
        check('switches idle', console.command('M114\n', 10),
              'LIMIT1:0 LIMIT2:0 THETA_INDEX:0 HOMED:0')

        # Homing seeks the inner switch, backs off, approaches slowly, then
        # finds the theta index tape.
        check('homing', console.command('G28\n', 120), 'ok')
        check('homed at the switch', console.command('M114\n', 10),
              'R:15.88 T:0.00 LIMIT1:1 LIMIT2:0 THETA_INDEX:1 HOMED:1')

        # A move must leave the switch behind and land where it was asked to.
        console.command('G1 X40 Y0 F600\n', 120)
        check('moved', console.command('M114\n', 10),
              'X:40.00 Y:0.00 R:40.00 T:0.00 LIMIT1:0')
    finally:
        console.close()

    print(f'\n{"FAILED: " + ", ".join(failures) if failures else "all checks passed"}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
