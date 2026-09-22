#!/usr/bin/env python3
"""Assemble and run the PIC18 instruction fixtures.

Each fixture exits through the test machine's device: 0 means every check
passed, any other value is the code of the check that failed. A fixture may
opt out of that with a directive on a comment line:

    ; expect: halt      the guest is expected to stop and never exit

The fixtures are assembled with XC8's pic-as for the PIC18F27Q43. The
assembler and its device family pack are looked for under the usual Windows
and Linux install locations, or given with PIC_AS and PIC18_DFP.

Usage: run-tests.py <path-to-qemu-system-pic18> [fixture.s ...]
"""

import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TIMEOUT = 20


def find_pic_as():
    if os.environ.get('PIC_AS'):
        return os.environ['PIC_AS']
    patterns = [
        'C:/Program Files/Microchip/xc8/v*/pic-as/bin/pic-as.exe',
        '/opt/microchip/xc8/v*/pic-as/bin/pic-as',
    ]
    found = []
    for pattern in patterns:
        found += glob.glob(pattern)
    if not found:
        sys.exit('pic-as not found; set PIC_AS')
    return sorted(found)[-1]


def find_dfp():
    if os.environ.get('PIC18_DFP'):
        return os.environ['PIC18_DFP']
    home = os.path.expanduser('~')
    found = glob.glob(os.path.join(home, '.mchp_packs', 'Microchip',
                                   'PIC18F-Q_DFP', '*', 'xc8'))
    if not found:
        sys.exit('PIC18F-Q device family pack not found; set PIC18_DFP')
    return sorted(found)[-1]


def expectation(path):
    with open(path) as f:
        for line in f:
            m = re.match(r'^\s*;\s*expect:\s*(\w+)', line)
            if m:
                return m.group(1)
    return 'exit0'


SUMMARY_LINES = ('Memory Summary', 'Program space', 'Data space',
                 'Configuration bits', 'EEPROM space', 'ID Location space')


def assemble(pic_as, dfp, source, workdir):
    hexfile = os.path.join(workdir, os.path.basename(source)[:-2] + '.hex')
    # pic-as resolves an "include" against the working directory, not -I.
    proc = subprocess.run(
        [pic_as, '-mcpu=18f27q43', '-mdfp=' + dfp, '-w',
         '-o', hexfile, os.path.abspath(source)],
        capture_output=True, text=True, cwd=HERE)
    noise = [l for l in (proc.stdout + proc.stderr).splitlines()
             if l.strip() and not l.strip().startswith(SUMMARY_LINES)
             and 'Memory Summary' not in l]
    if proc.returncode != 0 or not os.path.exists(hexfile):
        return None, '\n'.join(noise)
    return hexfile, '\n'.join(noise)


def run(qemu, hexfile, expect):
    cmd = [qemu, '-M', 'pic18-test', '-bios', hexfile,
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
        os.path.join(HERE, f) for f in os.listdir(HERE)
        if f.startswith('t_') and f.endswith('.s'))

    pic_as = find_pic_as()
    dfp = find_dfp()

    failures = 0
    with tempfile.TemporaryDirectory() as workdir:
        for source in sources:
            name = os.path.basename(source)
            hexfile, noise = assemble(pic_as, dfp, source, workdir)
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
