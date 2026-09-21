/*
 * PIC18 (Q43 core, standard instruction set) disassembler
 *
 * Table-driven and deliberately independent of insn.decode, so that the two
 * representations of the opcode map cross-check each other. The output
 * format follows XC8's pic-objdump so the two can be diffed directly:
 * operands as 0x.. hex, "w"/"f" for the destination, "a"/"b" for access
 * bank versus banked, and branch targets as absolute addresses.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"

typedef enum {
    FMT_NONE,   /* no operands */
    FMT_FDA,    /* f, d, a */
    FMT_FA,     /* f, a */
    FMT_FBA,    /* f, b, a */
    FMT_K8,     /* 8-bit literal */
    FMT_K6,     /* 6-bit literal (MOVLB) */
    FMT_MOVFF,  /* two words: 12-bit source, 12-bit destination */
    FMT_MOVFFL, /* three words: 14-bit source and destination */
    FMT_LFSR,   /* two words: fn, 14-bit literal */
    FMT_FSRK,   /* ADDFSR/SUBFSR: fn, 6-bit literal */
    FMT_BRA11,  /* 11-bit signed word displacement */
    FMT_BR8,    /* 8-bit signed word displacement */
    FMT_CALL,   /* two words: 20-bit word address, s */
    FMT_GOTO,   /* two words: 20-bit word address */
    FMT_S,      /* RETURN/RETFIE with the s bit */
    FMT_TBL,    /* TBLRD/TBLWT with the mode */
} PIC18Format;

typedef struct {
    uint16_t mask;
    uint16_t match;
    const char *mnemonic;
    PIC18Format format;
} PIC18Opcode;

/* Most specific first. */
static const PIC18Opcode pic18_opcodes[] = {
    { 0xFFFF, 0x0000, "nop",    FMT_NONE },
    { 0xFFFF, 0x0003, "sleep",  FMT_NONE },
    { 0xFFFF, 0x0004, "clrwdt", FMT_NONE },
    { 0xFFFF, 0x0005, "push",   FMT_NONE },
    { 0xFFFF, 0x0006, "pop",    FMT_NONE },
    { 0xFFFF, 0x0007, "daw",    FMT_NONE },
    { 0xFFFC, 0x0008, "tblrd",  FMT_TBL },
    { 0xFFFC, 0x000C, "tblwt",  FMT_TBL },
    { 0xFFFE, 0x0010, "retfie", FMT_S },
    { 0xFFFE, 0x0012, "return", FMT_S },
    { 0xFFFF, 0x0014, "callw",  FMT_NONE },
    { 0xFFF0, 0x0060, "movffl", FMT_MOVFFL },
    { 0xFFFF, 0x00FF, "reset",  FMT_NONE },
    { 0xFFC0, 0x0100, "movlb",  FMT_K6 },
    { 0xFE00, 0x0200, "mulwf",  FMT_FA },
    { 0xFC00, 0x0400, "decf",   FMT_FDA },
    { 0xFF00, 0x0800, "sublw",  FMT_K8 },
    { 0xFF00, 0x0900, "iorlw",  FMT_K8 },
    { 0xFF00, 0x0A00, "xorlw",  FMT_K8 },
    { 0xFF00, 0x0B00, "andlw",  FMT_K8 },
    { 0xFF00, 0x0C00, "retlw",  FMT_K8 },
    { 0xFF00, 0x0D00, "mullw",  FMT_K8 },
    { 0xFF00, 0x0E00, "movlw",  FMT_K8 },
    { 0xFF00, 0x0F00, "addlw",  FMT_K8 },
    { 0xFC00, 0x1000, "iorwf",  FMT_FDA },
    { 0xFC00, 0x1400, "andwf",  FMT_FDA },
    { 0xFC00, 0x1800, "xorwf",  FMT_FDA },
    { 0xFC00, 0x1C00, "comf",   FMT_FDA },
    { 0xFC00, 0x2000, "addwfc", FMT_FDA },
    { 0xFC00, 0x2400, "addwf",  FMT_FDA },
    { 0xFC00, 0x2800, "incf",   FMT_FDA },
    { 0xFC00, 0x2C00, "decfsz", FMT_FDA },
    { 0xFC00, 0x3000, "rrcf",   FMT_FDA },
    { 0xFC00, 0x3400, "rlcf",   FMT_FDA },
    { 0xFC00, 0x3800, "swapf",  FMT_FDA },
    { 0xFC00, 0x3C00, "incfsz", FMT_FDA },
    { 0xFC00, 0x4000, "rrncf",  FMT_FDA },
    { 0xFC00, 0x4400, "rlncf",  FMT_FDA },
    { 0xFC00, 0x4800, "infsnz", FMT_FDA },
    { 0xFC00, 0x4C00, "dcfsnz", FMT_FDA },
    { 0xFC00, 0x5000, "movf",   FMT_FDA },
    { 0xFC00, 0x5400, "subfwb", FMT_FDA },
    { 0xFC00, 0x5800, "subwfb", FMT_FDA },
    { 0xFC00, 0x5C00, "subwf",  FMT_FDA },
    { 0xFE00, 0x6000, "cpfslt", FMT_FA },
    { 0xFE00, 0x6200, "cpfseq", FMT_FA },
    { 0xFE00, 0x6400, "cpfsgt", FMT_FA },
    { 0xFE00, 0x6600, "tstfsz", FMT_FA },
    { 0xFE00, 0x6800, "setf",   FMT_FA },
    { 0xFE00, 0x6A00, "clrf",   FMT_FA },
    { 0xFE00, 0x6C00, "negf",   FMT_FA },
    { 0xFE00, 0x6E00, "movwf",  FMT_FA },
    { 0xF000, 0x7000, "btg",    FMT_FBA },
    { 0xF000, 0x8000, "bsf",    FMT_FBA },
    { 0xF000, 0x9000, "bcf",    FMT_FBA },
    { 0xF000, 0xA000, "btfss",  FMT_FBA },
    { 0xF000, 0xB000, "btfsc",  FMT_FBA },
    { 0xF000, 0xC000, "movff",  FMT_MOVFF },
    { 0xF800, 0xD000, "bra",    FMT_BRA11 },
    { 0xF800, 0xD800, "rcall",  FMT_BRA11 },
    { 0xFF00, 0xE000, "bz",     FMT_BR8 },
    { 0xFF00, 0xE100, "bnz",    FMT_BR8 },
    { 0xFF00, 0xE200, "bc",     FMT_BR8 },
    { 0xFF00, 0xE300, "bnc",    FMT_BR8 },
    { 0xFF00, 0xE400, "bov",    FMT_BR8 },
    { 0xFF00, 0xE500, "bnov",   FMT_BR8 },
    { 0xFF00, 0xE600, "bn",     FMT_BR8 },
    { 0xFF00, 0xE700, "bnn",    FMT_BR8 },
    { 0xFFC0, 0xE800, "addfsr", FMT_FSRK },
    { 0xFFC0, 0xE840, "addfsr", FMT_FSRK },
    { 0xFFC0, 0xE880, "addfsr", FMT_FSRK },
    { 0xFFC0, 0xE900, "subfsr", FMT_FSRK },
    { 0xFFC0, 0xE940, "subfsr", FMT_FSRK },
    { 0xFFC0, 0xE980, "subfsr", FMT_FSRK },
    { 0xFE00, 0xEC00, "call",   FMT_CALL },
    { 0xFFF0, 0xEE00, "lfsr",   FMT_LFSR },
    { 0xFFF0, 0xEE10, "lfsr",   FMT_LFSR },
    { 0xFFF0, 0xEE20, "lfsr",   FMT_LFSR },
    { 0xFF00, 0xEF00, "goto",   FMT_GOTO },
    /* The continuation word of a multi-word instruction, on its own. */
    { 0xF000, 0xF000, "nop",    FMT_NONE },
};

static const char *const pic18_tbl_modes[4] = { "*", "*+", "*-", "+*" };

static int read_word(bfd_vma addr, disassemble_info *info, uint32_t *word)
{
    bfd_byte buffer[2];
    int status = info->read_memory_func(addr, buffer, 2, info);

    if (status != 0) {
        info->memory_error_func(status, addr, info);
        return -1;
    }
    *word = bfd_getl16(buffer);
    return 0;
}

int pic18_print_insn(bfd_vma addr, disassemble_info *info)
{
    uint32_t insn, w2, w3;
    int i;

    if (read_word(addr, info, &insn) < 0) {
        return -1;
    }

    for (i = 0; i < ARRAY_SIZE(pic18_opcodes); i++) {
        const PIC18Opcode *op = &pic18_opcodes[i];
        int f = insn & 0xFF;
        const char *acc = (insn & 0x100) ? "b" : "a";
        const char *dst = (insn & 0x200) ? "f" : "w";
        int k;

        if ((insn & op->mask) != op->match) {
            continue;
        }

        switch (op->format) {
        case FMT_NONE:
            info->fprintf_func(info->stream, "%s", op->mnemonic);
            return 2;
        case FMT_FDA:
            info->fprintf_func(info->stream, "%s\t0x%x, %s, %s",
                               op->mnemonic, f, dst, acc);
            return 2;
        case FMT_FA:
            info->fprintf_func(info->stream, "%s\t0x%x, %s",
                               op->mnemonic, f, acc);
            return 2;
        case FMT_FBA:
            info->fprintf_func(info->stream, "%s\t0x%x, 0x%x, %s",
                               op->mnemonic, f, (insn >> 9) & 7, acc);
            return 2;
        case FMT_K8:
            info->fprintf_func(info->stream, "%s\t0x%x",
                               op->mnemonic, insn & 0xFF);
            return 2;
        case FMT_K6:
            info->fprintf_func(info->stream, "%s\t0x%x",
                               op->mnemonic, insn & 0x3F);
            return 2;
        case FMT_MOVFF:
            if (read_word(addr + 2, info, &w2) < 0) {
                return -1;
            }
            info->fprintf_func(info->stream, "%s\t0x%x, 0x%x",
                               op->mnemonic, insn & 0xFFF, w2 & 0xFFF);
            return 4;
        case FMT_MOVFFL:
            if (read_word(addr + 2, info, &w2) < 0 ||
                read_word(addr + 4, info, &w3) < 0) {
                return -1;
            }
            info->fprintf_func(info->stream, "%s\t0x%x, 0x%x", op->mnemonic,
                               ((insn & 0xF) << 10) | ((w2 >> 2) & 0x3FF),
                               ((w2 & 3) << 12) | (w3 & 0xFFF));
            return 6;
        case FMT_LFSR:
            if (read_word(addr + 2, info, &w2) < 0) {
                return -1;
            }
            info->fprintf_func(info->stream, "%s\t0x%x, 0x%x", op->mnemonic,
                               (insn >> 4) & 3,
                               ((insn & 0xF) << 10) | (w2 & 0x3FF));
            return 4;
        case FMT_FSRK:
            info->fprintf_func(info->stream, "%s\t0x%x, 0x%x", op->mnemonic,
                               (insn >> 6) & 3, insn & 0x3F);
            return 2;
        case FMT_BRA11:
            k = insn & 0x7FF;
            if (k & 0x400) {
                k -= 0x800;
            }
            info->fprintf_func(info->stream, "%s\t0x%x", op->mnemonic,
                               (uint32_t)(addr + 2 + 2 * k) & PIC18_PC_MASK);
            return 2;
        case FMT_BR8:
            k = insn & 0xFF;
            if (k & 0x80) {
                k -= 0x100;
            }
            info->fprintf_func(info->stream, "%s\t0x%x", op->mnemonic,
                               (uint32_t)(addr + 2 + 2 * k) & PIC18_PC_MASK);
            return 2;
        case FMT_CALL:
            if (read_word(addr + 2, info, &w2) < 0) {
                return -1;
            }
            k = ((insn & 0xFF) | ((w2 & 0xFFF) << 8)) << 1;
            if (insn & 0x100) {
                info->fprintf_func(info->stream, "%s\t0x%x, 1",
                                   op->mnemonic, k);
            } else {
                info->fprintf_func(info->stream, "%s\t0x%x", op->mnemonic, k);
            }
            return 4;
        case FMT_GOTO:
            if (read_word(addr + 2, info, &w2) < 0) {
                return -1;
            }
            k = ((insn & 0xFF) | ((w2 & 0xFFF) << 8)) << 1;
            info->fprintf_func(info->stream, "%s\t0x%x", op->mnemonic, k);
            return 4;
        case FMT_S:
            if (insn & 1) {
                info->fprintf_func(info->stream, "%s\t1", op->mnemonic);
            } else {
                info->fprintf_func(info->stream, "%s", op->mnemonic);
            }
            return 2;
        case FMT_TBL:
            info->fprintf_func(info->stream, "%s\t%s", op->mnemonic,
                               pic18_tbl_modes[insn & 3]);
            return 2;
        default:
            g_assert_not_reached();
        }
    }

    /* Undefined on hardware too (the extended set, or reserved). */
    info->fprintf_func(info->stream, ".word\t0x%04x", insn);
    return 2;
}
