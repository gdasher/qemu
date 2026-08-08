#!/usr/bin/env python3
#
# Recover the PIC32 special function register map from an XC32-built ELF.
#
# XC32 declares every SFR as an absolute symbol, so a firmware image built for
# a part carries that part's whole register map -- names, addresses, and which
# registers have the atomic CLR/SET/INV aliases. That is more reliable than
# transcribing the data sheet's memory map tables, and it is what the device
# models in hw/pic32/ are written against.
#
#   ./sfrmap.py firmware.elf                     module bases and sizes
#   ./sfrmap.py firmware.elf --list 'SPI1.*'     registers matching a pattern
#   ./sfrmap.py firmware.elf --module SPI1       one module, as offsets
#   ./sfrmap.py firmware.elf --defines SPI1      the same, as C defines
#   ./sfrmap.py firmware.elf --json              everything, for a test
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import json
import re
import struct
import sys

# The SFR window, physical. Symbols are given in KSEG1.
SFR_BASE = 0x1F800000
SFR_SIZE = 0x00100000

# The three atomic aliases every writable SFR has, at +4, +8 and +0xC.
ALIASES = ('CLR', 'SET', 'INV')


def read_symbols(path):
    """Yield (name, value) for every ELF32 symbol, without binutils."""
    with open(path, 'rb') as f:
        data = f.read()

    if data[:4] != b'\x7fELF' or data[4] != 1:
        raise ValueError('%s is not a 32-bit ELF' % path)
    little = data[5] == 1
    end = '<' if little else '>'

    e_shoff, = struct.unpack_from(end + 'I', data, 0x20)
    e_shentsize, e_shnum = struct.unpack_from(end + 'HH', data, 0x2E)

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type, = struct.unpack_from(end + 'I', data, off + 4)
        sh_offset, sh_size, sh_link = struct.unpack_from(end + 'III',
                                                         data, off + 0x10)
        sections.append((sh_type, sh_offset, sh_size, sh_link))

    for sh_type, sh_offset, sh_size, sh_link in sections:
        if sh_type != 2:  # SHT_SYMTAB
            continue
        _, str_off, str_size, _ = sections[sh_link]
        strtab = data[str_off:str_off + str_size]
        for off in range(sh_offset, sh_offset + sh_size, 16):
            st_name, st_value = struct.unpack_from(end + 'II', data, off)
            if st_name == 0:
                continue
            name_end = strtab.index(b'\0', st_name)
            yield strtab[st_name:name_end].decode(), st_value


def read_sfrs(path):
    """The SFR symbols, as {name: physical address}."""
    sfrs = {}
    for name, value in read_symbols(path):
        phys = value & 0x1FFFFFFF
        if SFR_BASE <= phys < SFR_BASE + SFR_SIZE:
            sfrs[name] = phys
    if not sfrs:
        raise ValueError('%s has no SFR symbols -- not an XC32 image?' % path)
    return sfrs


def fold_aliases(sfrs):
    """
    Separate the base registers from their CLR/SET/INV aliases, checking that
    each alias sits where the convention says it does. Returns the base
    registers as {name: (address, has_aliases)} plus a list of complaints.
    """
    bases, problems = {}, []

    for name, addr in sfrs.items():
        if any(name.endswith(a) for a in ALIASES):
            continue
        aliased = None
        for i, alias in enumerate(ALIASES):
            want = sfrs.get(name + alias)
            if want is None:
                if aliased:
                    problems.append('%s has %s aliases but no %s'
                                    % (name, '/'.join(ALIASES[:i]), alias))
                aliased = False
            elif want != addr + 4 * (i + 1):
                problems.append('%s%s is at 0x%08X, not 0x%08X'
                                % (name, alias, want, addr + 4 * (i + 1)))
                aliased = True
            else:
                if aliased is False:
                    problems.append('%s has a %s alias but not the earlier ones'
                                    % (name, alias))
                aliased = True
        bases[name] = (addr, bool(aliased))

    # Anything ending in CLR/SET/INV that is not an alias is a register in its
    # own right, and would be missed by the loop above.
    for name in sfrs:
        for alias in ALIASES:
            if name.endswith(alias) and name[:-3] not in sfrs:
                bases[name] = (sfrs[name], False)

    return bases, problems


# Registers this far apart belong to different blocks. Within a block PIC32
# strides 0x10 for an aliased register and 4 for the PPS ones; between blocks
# the gap is at least 0x100 on every module in this part.
BLOCK_GAP = 0x80


def block_name(names):
    """
    What to call a block, from the names in it. The longest common prefix is
    right for most modules -- U1MODE/U1STA/U1BRG gives U1, SPI3CON/SPI3STAT
    gives SPI3 -- but it collapses to nothing for blocks whose registers are
    named per-port or per-source, so those fall back to the first name.
    """
    prefix = names[0]
    for name in names[1:]:
        while not name.startswith(prefix):
            prefix = prefix[:-1]
    prefix = prefix.rstrip('_')
    if len(prefix) < 2:
        # No shared prefix: name it after the first register, minus any
        # trailing index, which is how EVIC (IFS0...) and PPS (INT1R...) read.
        return re.sub(r'\d+$', '', names[0]) or names[0]
    return prefix


def group(bases):
    """
    {block: [(name, address, aliased)]}, clustered by address rather than by
    name. Name-based grouping cannot work on this part: the USB registers and
    the UART1 registers are both called U1something, and they are a quarter of
    a megabyte apart.
    """
    regs = sorted(((a, n, x) for n, (a, x) in bases.items()))

    blocks, current = [], []
    for addr, name, aliased in regs:
        if current and addr - current[-1][0] > BLOCK_GAP:
            blocks.append(current)
            current = []
        current.append((addr, name, aliased))
    if current:
        blocks.append(current)

    out = {}
    for block in blocks:
        name = block_name([n for _, n, _ in block])
        if name in out:
            name = '%s@%08X' % (name, block[0][0])
        out[name] = [(n, a, x) for a, n, x in block]
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('elf')
    p.add_argument('--list', metavar='REGEX',
                   help='registers whose name matches')
    p.add_argument('--module', metavar='NAME',
                   help='one module, as offsets from its base')
    p.add_argument('--defines', metavar='NAME',
                   help='one module, as C defines')
    p.add_argument('--json', action='store_true', help='everything, as JSON')
    args = p.parse_args()

    sfrs = read_sfrs(args.elf)
    bases, problems = fold_aliases(sfrs)
    modules = group(bases)

    for complaint in problems:
        print('warning: ' + complaint, file=sys.stderr)

    if args.json:
        json.dump({m: [{'name': n, 'addr': a, 'aliased': x}
                       for n, a, x in regs]
                   for m, regs in modules.items()},
                  sys.stdout, indent=1, sort_keys=True)
        print()
        return 0

    if args.list:
        pattern = re.compile(args.list + '$')
        for name, (addr, aliased) in sorted(bases.items(),
                                            key=lambda kv: kv[1][0]):
            if pattern.match(name):
                print('0x%08X  %-12s %s' % (addr, name,
                                            '+CLR/SET/INV' if aliased else ''))
        return 0

    name = args.module or args.defines
    if name:
        regs = modules.get(name)
        if not regs:
            print("no module '%s'; try --json" % name, file=sys.stderr)
            return 1
        base = regs[0][1]
        if args.defines:
            print('#define %s_BASE 0x%08X' % (name.upper(), base))
            for reg, addr, aliased in regs:
                print('#define %-16s 0x%03X%s'
                      % ('R_' + reg, addr - base,
                         '  /* +CLR/SET/INV */' if aliased else ''))
        else:
            print('%s at 0x%08X' % (name, base))
            for reg, addr, aliased in regs:
                print('  +0x%03X  %-12s %s'
                      % (addr - base, reg, '+CLR/SET/INV' if aliased else ''))
        return 0

    print('%-10s %-12s %-8s %s' % ('module', 'base', 'span', 'registers'))
    for module, regs in sorted(modules.items(), key=lambda kv: kv[1][0][1]):
        base, last = regs[0][1], regs[-1][1]
        print('%-10s 0x%08X   0x%04X   %d'
              % (module, base, last - base + 4, len(regs)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
