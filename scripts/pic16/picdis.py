#!/usr/bin/env python3
#
# Reference disassembler and opcode-table validator for the PIC16 enhanced
# mid-range core (PIC16F1xxxx), used while bringing up target/pic16.
#
# Two uses:
#   picdis.py validate <image.hex>          coverage + mnemonic histogram
#   picdis.py dis <image.hex> <addr> <n>    disassemble n words from addr
#
# The opcode table below was transcribed from the PIC16F17526/46 data sheet
# (DS40002637A) Table 45-3 and then validated against a shipped XC8 image.
# Two entries in that table are wrong as printed; see ERRATA.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import collections
import sys

# ERRATA against DS40002637A Table 45-3:
#
# MOVLB  The table prints a malformed 11-bit pattern ("00 000 0k kkkk") and the
#        detail page (DS p.663) gives no encoding at all, while stating a 6-bit
#        literal. The real encoding is 00 0001 01kk kkkk (0x0140-0x017F), which
#        occupies the gap between CLRW (0x0100) and CLRF (0x0180). Established
#        empirically: a 5-bit literal cannot reach bank 60, where this family
#        keeps the PPS input registers, and 0x0140-0x017F is the most common
#        opcode class in a real image with a bank distribution matching the
#        firmware's register use.
#
# BTFSS  The table prints a 4-bit MSb cell ("1010 11bb bfff ffff"), 18 bits in
#        total. The encoding is 01 11bb bfff ffff, completing the regular
#        BCF/BSF/BTFSC/BTFSS progression 0100/0101/0110/0111.

# (mask, match, mnemonic) -- most specific first.
TABLE = [
    (0x3FFF, 0x0000, "NOP"),
    (0x3FFF, 0x0001, "RESET"),
    (0x3FFF, 0x0008, "RETURN"),
    (0x3FFF, 0x0009, "RETFIE"),
    (0x3FFF, 0x000A, "CALLW"),
    (0x3FFF, 0x000B, "BRW"),
    (0x3FF8, 0x0010, "MOVIW n,mm"),
    (0x3FF8, 0x0018, "MOVWI n,mm"),
    (0x3FFF, 0x0063, "SLEEP"),
    (0x3FFF, 0x0064, "CLRWDT"),
    (0x3FF8, 0x0060, "TRIS f"),
    (0x3F80, 0x0080, "MOVWF f"),
    (0x3FFC, 0x0100, "CLRW"),
    (0x3FC0, 0x0140, "MOVLB k"),
    (0x3F80, 0x0180, "CLRF f"),
    (0x3F00, 0x0200, "SUBWF f,d"),
    (0x3F00, 0x0300, "DECF f,d"),
    (0x3F00, 0x0400, "IORWF f,d"),
    (0x3F00, 0x0500, "ANDWF f,d"),
    (0x3F00, 0x0600, "XORWF f,d"),
    (0x3F00, 0x0700, "ADDWF f,d"),
    (0x3F00, 0x0800, "MOVF f,d"),
    (0x3F00, 0x0900, "COMF f,d"),
    (0x3F00, 0x0A00, "INCF f,d"),
    (0x3F00, 0x0B00, "DECFSZ f,d"),
    (0x3F00, 0x0C00, "RRF f,d"),
    (0x3F00, 0x0D00, "RLF f,d"),
    (0x3F00, 0x0E00, "SWAPF f,d"),
    (0x3F00, 0x0F00, "INCFSZ f,d"),
    (0x3C00, 0x1000, "BCF f,b"),
    (0x3C00, 0x1400, "BSF f,b"),
    (0x3C00, 0x1800, "BTFSC f,b"),
    (0x3C00, 0x1C00, "BTFSS f,b"),
    (0x3800, 0x2000, "CALL k"),
    (0x3800, 0x2800, "GOTO k"),
    (0x3F00, 0x3000, "MOVLW k"),
    (0x3F80, 0x3100, "ADDFSR n,k"),
    (0x3F80, 0x3180, "MOVLP k"),
    (0x3E00, 0x3200, "BRA k"),
    (0x3F00, 0x3400, "RETLW k"),
    (0x3F00, 0x3500, "LSLF f,d"),
    (0x3F00, 0x3600, "LSRF f,d"),
    (0x3F00, 0x3700, "ASRF f,d"),
    (0x3F00, 0x3800, "IORLW k"),
    (0x3F00, 0x3900, "ANDLW k"),
    (0x3F00, 0x3A00, "XORLW k"),
    (0x3F00, 0x3B00, "SUBWFB f,d"),
    (0x3F00, 0x3C00, "SUBLW k"),
    (0x3F00, 0x3D00, "ADDWFC f,d"),
    (0x3F00, 0x3E00, "ADDLW k"),
    (0x3F80, 0x3F00, "MOVIW k[n]"),
    (0x3F80, 0x3F80, "MOVWI k[n]"),
]

MNEMONICS = {m.split()[0] for _, _, m in TABLE}

# MOVIW/MOVWI addressing modes (DS p.662).
MODES = {0: "++FSR%d", 1: "--FSR%d", 2: "FSR%d++", 3: "FSR%d--"}

FD = ("SUBWF DECF IORWF ANDWF XORWF ADDWF MOVF COMF INCF DECFSZ RRF RLF "
      "SWAPF INCFSZ LSLF LSRF ASRF SUBWFB ADDWFC").split()
LIT = "MOVLW RETLW IORLW ANDLW XORLW SUBLW ADDLW".split()


def decode(word):
    for mask, match, mnemonic in TABLE:
        if (word & mask) == match:
            return mnemonic
    return None


def disassemble(word):
    mnemonic = decode(word)
    if mnemonic is None:
        return f"??? 0x{word:04X}"
    op = mnemonic.split()[0]
    f = word & 0x7F
    if op in FD:
        return f"{op:7s} 0x{f:02X}, {(word >> 7) & 1}"
    if op in ("MOVWF", "CLRF"):
        return f"{op:7s} 0x{f:02X}"
    if op in ("BCF", "BSF", "BTFSC", "BTFSS"):
        return f"{op:7s} 0x{f:02X}, {(word >> 7) & 7}"
    if op in LIT:
        return f"{op:7s} 0x{word & 0xFF:02X}"
    if op == "MOVLB":
        return f"{op:7s} {word & 0x3F}"
    if op == "MOVLP":
        return f"{op:7s} 0x{word & 0x7F:02X}"
    if op in ("CALL", "GOTO"):
        return f"{op:7s} 0x{word & 0x7FF:04X}"
    if op == "BRA":
        off = word & 0x1FF
        return f"{op:7s} {off - 512 if off & 0x100 else off:+d}"
    if op == "ADDFSR":
        k = word & 0x3F
        return f"{op:7s} FSR{(word >> 6) & 1}, {k - 64 if k & 0x20 else k:+d}"
    if op in ("MOVIW", "MOVWI"):
        if (word & 0x3000) == 0:
            return f"{op:7s} {MODES[word & 3] % ((word >> 2) & 1)}"
        k = word & 0x3F
        return f"{op:7s} {k - 64 if k & 0x20 else k}[FSR{(word >> 6) & 1}]"
    if op == "TRIS":
        return f"{op:7s} {word & 7}"
    return op


def load_hex(path):
    """Intel HEX -> {byte address: value}. XC8 emits program memory at byte
    address = word address * 2, and the config words above 0x10000."""
    mem, base = {}, 0
    for line in open(path):
        line = line.strip()
        if not line.startswith(':'):
            continue
        raw = bytes.fromhex(line[1:])
        count, addr, kind = raw[0], (raw[1] << 8) | raw[2], raw[3]
        data = raw[4:4 + count]
        if kind == 0:
            for i, value in enumerate(data):
                mem[base + addr + i] = value
        elif kind == 4:
            base = ((data[0] << 8) | data[1]) << 16
        elif kind == 1:
            break
    return mem


def load_words(path):
    mem = load_hex(path)
    return {a // 2: mem[a] | (mem.get(a + 1, 0) << 8)
            for a in range(0, 0x8000, 2) if a in mem}


def validate(path):
    words = load_words(path)
    wide = [a for a, w in words.items() if w > 0x3FFF]
    print(f"words: {len(words)}   over 14 bits: {len(wide)}")

    hist, undecoded = collections.Counter(), []
    for addr, word in sorted(words.items()):
        mnemonic = decode(word)
        if mnemonic is None:
            undecoded.append((addr, word))
        else:
            hist[mnemonic.split()[0]] += 1

    print(f"decoded: {len(words) - len(undecoded)}   undecoded: {len(undecoded)}")
    for addr, word in undecoded[:20]:
        print(f"  0x{addr:04X}: 0x{word:04X}")
    print(f"\nmnemonics exercised: {len(hist)} of {len(MNEMONICS)}")
    for mnemonic, n in hist.most_common():
        print(f"  {mnemonic:8s} x{n}")
    print("\nnot exercised:", ", ".join(sorted(MNEMONICS - set(hist))) or "(none)")
    return 1 if undecoded or wide else 0


def main(argv):
    if len(argv) < 3:
        print(__doc__ or "usage: picdis.py validate|dis <image.hex> ...")
        return 2
    if argv[1] == "validate":
        return validate(argv[2])
    if argv[1] == "dis":
        words = load_words(argv[2])
        start, count = int(argv[3], 0), int(argv[4], 0)
        for addr in range(start, start + count):
            if addr in words:
                print(f"  0x{addr:04X}: 0x{words[addr]:04X}  "
                      f"{disassemble(words[addr])}")
        return 0
    print(f"unknown command: {argv[1]}")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
