/*
 * PIC16 (enhanced mid-range) disassembler
 *
 * Table-driven and deliberately independent of insn.decode, so that the two
 * representations of the opcode map cross-check each other. The same table,
 * validated against shipped XC8 images, lives in scripts/pic16/picdis.py;
 * see that file for the two errata against DS40002637A Table 45-3.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"

typedef enum {
    FMT_NONE,   /* no operands */
    FMT_FD,     /* f, d */
    FMT_F,      /* f */
    FMT_FB,     /* f, b */
    FMT_K8,     /* 8-bit literal */
    FMT_K7,     /* 7-bit literal (MOVLP) */
    FMT_K6,     /* 6-bit literal (MOVLB) */
    FMT_K11,    /* 11-bit address (CALL, GOTO) */
    FMT_BRA,    /* 9-bit signed displacement */
    FMT_FSRK,   /* ADDFSR: FSRn, signed 6-bit */
    FMT_MM,     /* MOVIW/MOVWI with pre/post inc/dec */
    FMT_IDX,    /* MOVIW/MOVWI indexed: k[FSRn] */
    FMT_TRIS,   /* TRIS f */
} PIC16Format;

typedef struct {
    uint16_t mask;
    uint16_t match;
    const char *mnemonic;
    PIC16Format format;
} PIC16Opcode;

/* Most specific first. */
static const PIC16Opcode pic16_opcodes[] = {
    { 0x3FFF, 0x0000, "nop",    FMT_NONE },
    { 0x3FFF, 0x0001, "reset",  FMT_NONE },
    { 0x3FFF, 0x0008, "return", FMT_NONE },
    { 0x3FFF, 0x0009, "retfie", FMT_NONE },
    { 0x3FFF, 0x000A, "callw",  FMT_NONE },
    { 0x3FFF, 0x000B, "brw",    FMT_NONE },
    { 0x3FF8, 0x0010, "moviw",  FMT_MM },
    { 0x3FF8, 0x0018, "movwi",  FMT_MM },
    { 0x3FFF, 0x0063, "sleep",  FMT_NONE },
    { 0x3FFF, 0x0064, "clrwdt", FMT_NONE },
    { 0x3FF8, 0x0060, "tris",   FMT_TRIS },
    { 0x3F80, 0x0080, "movwf",  FMT_F },
    { 0x3FFC, 0x0100, "clrw",   FMT_NONE },
    { 0x3FC0, 0x0140, "movlb",  FMT_K6 },
    { 0x3F80, 0x0180, "clrf",   FMT_F },
    { 0x3F00, 0x0200, "subwf",  FMT_FD },
    { 0x3F00, 0x0300, "decf",   FMT_FD },
    { 0x3F00, 0x0400, "iorwf",  FMT_FD },
    { 0x3F00, 0x0500, "andwf",  FMT_FD },
    { 0x3F00, 0x0600, "xorwf",  FMT_FD },
    { 0x3F00, 0x0700, "addwf",  FMT_FD },
    { 0x3F00, 0x0800, "movf",   FMT_FD },
    { 0x3F00, 0x0900, "comf",   FMT_FD },
    { 0x3F00, 0x0A00, "incf",   FMT_FD },
    { 0x3F00, 0x0B00, "decfsz", FMT_FD },
    { 0x3F00, 0x0C00, "rrf",    FMT_FD },
    { 0x3F00, 0x0D00, "rlf",    FMT_FD },
    { 0x3F00, 0x0E00, "swapf",  FMT_FD },
    { 0x3F00, 0x0F00, "incfsz", FMT_FD },
    { 0x3C00, 0x1000, "bcf",    FMT_FB },
    { 0x3C00, 0x1400, "bsf",    FMT_FB },
    { 0x3C00, 0x1800, "btfsc",  FMT_FB },
    { 0x3C00, 0x1C00, "btfss",  FMT_FB },
    { 0x3800, 0x2000, "call",   FMT_K11 },
    { 0x3800, 0x2800, "goto",   FMT_K11 },
    { 0x3F00, 0x3000, "movlw",  FMT_K8 },
    { 0x3F80, 0x3100, "addfsr", FMT_FSRK },
    { 0x3F80, 0x3180, "movlp",  FMT_K7 },
    { 0x3E00, 0x3200, "bra",    FMT_BRA },
    { 0x3F00, 0x3400, "retlw",  FMT_K8 },
    { 0x3F00, 0x3500, "lslf",   FMT_FD },
    { 0x3F00, 0x3600, "lsrf",   FMT_FD },
    { 0x3F00, 0x3700, "asrf",   FMT_FD },
    { 0x3F00, 0x3800, "iorlw",  FMT_K8 },
    { 0x3F00, 0x3900, "andlw",  FMT_K8 },
    { 0x3F00, 0x3A00, "xorlw",  FMT_K8 },
    { 0x3F00, 0x3B00, "subwfb", FMT_FD },
    { 0x3F00, 0x3C00, "sublw",  FMT_K8 },
    { 0x3F00, 0x3D00, "addwfc", FMT_FD },
    { 0x3F00, 0x3E00, "addlw",  FMT_K8 },
    { 0x3F80, 0x3F00, "moviw",  FMT_IDX },
    { 0x3F80, 0x3F80, "movwi",  FMT_IDX },
};

/* MOVIW/MOVWI addressing modes, indexed by the mm field (DS40002637A p.662). */
static const char *const pic16_modes[4] = {
    "++fsr%d", "--fsr%d", "fsr%d++", "fsr%d--"
};

int pic16_print_insn(bfd_vma addr, disassemble_info *info)
{
    bfd_byte buffer[2];
    uint16_t insn;
    int status, i;

    status = info->read_memory_func(addr, buffer, 2, info);
    if (status != 0) {
        info->memory_error_func(status, addr, info);
        return -1;
    }
    insn = bfd_getl16(buffer) & 0x3FFF;

    for (i = 0; i < ARRAY_SIZE(pic16_opcodes); i++) {
        const PIC16Opcode *op = &pic16_opcodes[i];
        int f = insn & 0x7F;
        int k;

        if ((insn & op->mask) != op->match) {
            continue;
        }

        switch (op->format) {
        case FMT_NONE:
            info->fprintf_func(info->stream, "%s", op->mnemonic);
            break;
        case FMT_FD:
            info->fprintf_func(info->stream, "%-7s 0x%02x, %d",
                               op->mnemonic, f, (insn >> 7) & 1);
            break;
        case FMT_F:
            info->fprintf_func(info->stream, "%-7s 0x%02x", op->mnemonic, f);
            break;
        case FMT_FB:
            info->fprintf_func(info->stream, "%-7s 0x%02x, %d",
                               op->mnemonic, f, (insn >> 7) & 7);
            break;
        case FMT_K8:
            info->fprintf_func(info->stream, "%-7s 0x%02x",
                               op->mnemonic, insn & 0xFF);
            break;
        case FMT_K7:
            info->fprintf_func(info->stream, "%-7s 0x%02x",
                               op->mnemonic, insn & 0x7F);
            break;
        case FMT_K6:
            info->fprintf_func(info->stream, "%-7s %d",
                               op->mnemonic, insn & 0x3F);
            break;
        case FMT_K11:
            info->fprintf_func(info->stream, "%-7s 0x%04x",
                               op->mnemonic, insn & 0x7FF);
            break;
        case FMT_BRA:
            k = insn & 0x1FF;
            info->fprintf_func(info->stream, "%-7s %+d",
                               op->mnemonic, k & 0x100 ? k - 512 : k);
            break;
        case FMT_FSRK:
            k = insn & 0x3F;
            info->fprintf_func(info->stream, "%-7s fsr%d, %+d", op->mnemonic,
                               (insn >> 6) & 1, k & 0x20 ? k - 64 : k);
            break;
        case FMT_MM:
            info->fprintf_func(info->stream, "%-7s ", op->mnemonic);
            info->fprintf_func(info->stream, pic16_modes[insn & 3],
                               (insn >> 2) & 1);
            break;
        case FMT_IDX:
            k = insn & 0x3F;
            info->fprintf_func(info->stream, "%-7s %d[fsr%d]", op->mnemonic,
                               k & 0x20 ? k - 64 : k, (insn >> 6) & 1);
            break;
        case FMT_TRIS:
            info->fprintf_func(info->stream, "%-7s %d",
                               op->mnemonic, insn & 7);
            break;
        default:
            g_assert_not_reached();
        }
        return 2;
    }

    /* Every 14-bit word decodes on hardware; a gap here is a table bug. */
    info->fprintf_func(info->stream, ".word   0x%04x", insn);
    return 2;
}
