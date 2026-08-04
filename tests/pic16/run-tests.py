#!/usr/bin/env python3
"""Assemble and run the PIC16 instruction fixtures.

Each fixture exits through the test machine's device: 0 means every check
passed, any other value is the code of the check that failed. A fixture may
opt out of that with a directive on a comment line:

    ; expect: halt      the guest is expected to stop and never exit

Usage: run-tests.py <path-to-qemu-system-pic16> [fixture.asm ...]
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TIMEOUT = 20


def expectation(path):
    with open(path) as f:
        for line in f:
            m = re.match(r'^\s*;\s*expect:\s*(\w+)', line)
            if m:
                return m.group(1)
    return 'exit0'


def assemble(source, workdir):
    hexfile = os.path.join(workdir, os.path.basename(source)[:-4] + '.hex')
    proc = subprocess.run(
        ['gpasm', '-p', '16f1829', '-I', HERE, '-o', hexfile, source],
        capture_output=True, text=True)
    # gpasm warns that the command line supersedes the processor directive.
    noise = [l for l in proc.stderr.splitlines()
             if l.strip() and 'Processor superseded' not in l]
    if proc.returncode != 0:
        return None, '\n'.join(noise) or proc.stdout
    return hexfile, '\n'.join(noise)


def run(qemu, hexfile, expect):
    cmd = [qemu, '-M', 'pic16-test', '-bios', hexfile,
           '-display', 'none', '-monitor', 'none', '-nodefaults']
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        if expect == 'halt':
            return True, 'halted as expected'
        return False, f'timed out after {TIMEOUT}s'

    if expect == 'halt':
        return False, f'expected to halt, exited with {proc.returncode}'
    if proc.returncode == 0:
        return True, 'ok'
    detail = proc.stderr.strip()
    return False, f'check {proc.returncode} failed' + (
        f'\n{detail}' if detail else '')


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    qemu = argv[1]
    sources = argv[2:] or sorted(
        os.path.join(HERE, f) for f in os.listdir(HERE) if f.endswith('.asm'))

    failures = 0
    with tempfile.TemporaryDirectory() as workdir:
        for source in sources:
            name = os.path.basename(source)
            hexfile, noise = assemble(source, workdir)
            if hexfile is None:
                print(f'FAIL {name}: assembly failed\n{noise}')
                failures += 1
                continue
            if noise:
                print(f'     {name}: {noise}')
            ok, detail = run(qemu, hexfile, expectation(source))
            print(f'{"PASS" if ok else "FAIL"} {name}: {detail}')
            failures += not ok

    print(f'\n{len(sources) - failures}/{len(sources)} fixtures passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
