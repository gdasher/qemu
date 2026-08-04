/*
 * PIC16 (enhanced mid-range) translation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "tcg/tcg.h"
#include "cpu.h"
#include "exec/translation-block.h"
#include "tcg/tcg-op.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/log.h"
#include "exec/translator.h"
#include "exec/target_page.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef  HELPER_H

static TCGv cpu_pc;

static TCGv cpu_wreg;
static TCGv cpu_bsr;
static TCGv cpu_pclath;
static TCGv cpu_intcon;
static TCGv cpu_fsr[2];

static TCGv cpu_Cf;
static TCGv cpu_DCf;
static TCGv cpu_Zf;
static TCGv cpu_PDf;
static TCGv cpu_TOf;

#define PIC16_REG_OFFS(x) offsetof(CPUPIC16State, x)

typedef struct DisasContext {
    DisasContextBase base;

    CPUPIC16State *env;
    CPUState *cs;

    /*
     * Instruction addresses count words, while DisasContextBase works in
     * bytes; npc is the word address of the instruction after this one.
     */
    uint32_t npc;
} DisasContext;

/* Include the auto-generated decoder. */
static bool decode_insn(DisasContext *ctx, uint16_t insn);
#include "decode-insn.c.inc"

/*
 * Control flow
 */

static void gen_goto_tb(DisasContext *ctx, int n, uint32_t dest)
{
    if (translator_use_goto_tb(&ctx->base, dest * 2)) {
        tcg_gen_goto_tb(n);
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_exit_tb(ctx->base.tb, n);
    } else {
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_lookup_and_goto_ptr();
    }
}

static void gen_jump_direct(DisasContext *ctx, uint32_t dest)
{
    gen_goto_tb(ctx, 0, dest);
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_jump_indirect(DisasContext *ctx, TCGv dest)
{
    tcg_gen_andi_i32(cpu_pc, dest, 0x7FFF);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
}

/*
 * A skip is a conditional jump over the following instruction. Ending the
 * translation block here costs some chaining, but it keeps the skip entirely
 * within one instruction instead of carrying state into the next.
 */
static void gen_skip_if(DisasContext *ctx, TCGCond cond, TCGv val, int cmp)
{
    TCGLabel *no_skip = gen_new_label();

    tcg_gen_brcondi_i32(tcg_invert_cond(cond), val, cmp, no_skip);
    gen_goto_tb(ctx, 0, ctx->npc + 1);
    gen_set_label(no_skip);
    gen_goto_tb(ctx, 1, ctx->npc);
    ctx->base.is_jmp = DISAS_NORETURN;
}

/*
 * STATUS
 */

static void gen_status_read(TCGv dest)
{
    TCGv t = tcg_temp_new_i32();

    tcg_gen_shli_i32(dest, cpu_TOf, PIC16_STATUS_TO);
    tcg_gen_shli_i32(t, cpu_PDf, PIC16_STATUS_PD);
    tcg_gen_or_i32(dest, dest, t);
    tcg_gen_shli_i32(t, cpu_Zf, PIC16_STATUS_Z);
    tcg_gen_or_i32(dest, dest, t);
    tcg_gen_shli_i32(t, cpu_DCf, PIC16_STATUS_DC);
    tcg_gen_or_i32(dest, dest, t);
    tcg_gen_or_i32(dest, dest, cpu_Cf);
}

/* TO and PD are read-only. */
static void gen_status_write(TCGv src)
{
    tcg_gen_extract_i32(cpu_DCf, src, PIC16_STATUS_DC, 1);
    tcg_gen_extract_i32(cpu_Zf, src, PIC16_STATUS_Z, 1);
    tcg_gen_andi_i32(cpu_Cf, src, 1);
}

/*
 * File register access.
 *
 * Direct addressing supplies a 7-bit f. The low twelve addresses of every bank
 * are the core registers, which are bank-independent and live in
 * CPUPIC16State, so they resolve here rather than becoming memory accesses.
 * From 0x70 up is common RAM, which is aliased into every bank and so has a
 * constant address. Only 0x0C..0x6F actually depends on BSR.
 */

static TCGv gen_direct_addr(DisasContext *ctx, int f)
{
    TCGv addr = tcg_temp_new_i32();

    if (f >= PIC16_COMMON_BASE) {
        tcg_gen_movi_i32(addr, f);
    } else {
        tcg_gen_shli_i32(addr, cpu_bsr, 7);
        tcg_gen_ori_i32(addr, addr, f);
    }
    return addr;
}

static void gen_load_f(DisasContext *ctx, TCGv dest, int f)
{
    if (f >= PIC16_CORE_REGS) {
        tcg_gen_qemu_ld_tl(dest, gen_direct_addr(ctx, f), MMU_DATA_IDX, MO_UB);
        return;
    }

    switch (f) {
    case 0x00: /* INDF0 */
    case 0x01: /* INDF1 */
        gen_helper_ld_data(dest, tcg_env, cpu_fsr[f]);
        break;
    case 0x02: /* PCL: the PC has already advanced past this instruction */
        tcg_gen_movi_i32(dest, ctx->npc & 0xFF);
        break;
    case 0x03: /* STATUS */
        gen_status_read(dest);
        break;
    case 0x04: /* FSR0L */
        tcg_gen_andi_i32(dest, cpu_fsr[0], 0xFF);
        break;
    case 0x05: /* FSR0H */
        tcg_gen_extract_i32(dest, cpu_fsr[0], 8, 8);
        break;
    case 0x06: /* FSR1L */
        tcg_gen_andi_i32(dest, cpu_fsr[1], 0xFF);
        break;
    case 0x07: /* FSR1H */
        tcg_gen_extract_i32(dest, cpu_fsr[1], 8, 8);
        break;
    case 0x08: /* BSR */
        tcg_gen_mov_i32(dest, cpu_bsr);
        break;
    case 0x09: /* WREG */
        tcg_gen_mov_i32(dest, cpu_wreg);
        break;
    case 0x0A: /* PCLATH */
        tcg_gen_mov_i32(dest, cpu_pclath);
        break;
    case 0x0B: /* INTCON */
        tcg_gen_mov_i32(dest, cpu_intcon);
        break;
    default:
        g_assert_not_reached();
    }
}

static void gen_store_f(DisasContext *ctx, TCGv src, int f)
{
    if (f >= PIC16_CORE_REGS) {
        tcg_gen_qemu_st_tl(src, gen_direct_addr(ctx, f), MMU_DATA_IDX, MO_UB);
        return;
    }

    switch (f) {
    case 0x00: /* INDF0 */
    case 0x01: /* INDF1 */
        gen_helper_st_data(tcg_env, cpu_fsr[f], src);
        break;
    case 0x02: /* PCL: a write is a computed jump, high bits from PCLATH */
    {
        TCGv dest = tcg_temp_new_i32();

        tcg_gen_andi_i32(dest, cpu_pclath, 0x7F);
        tcg_gen_shli_i32(dest, dest, 8);
        tcg_gen_or_i32(dest, dest, src);
        gen_jump_indirect(ctx, dest);
        break;
    }
    case 0x03: /* STATUS */
        gen_status_write(src);
        break;
    case 0x04: /* FSR0L */
        tcg_gen_deposit_i32(cpu_fsr[0], cpu_fsr[0], src, 0, 8);
        break;
    case 0x05: /* FSR0H */
        tcg_gen_deposit_i32(cpu_fsr[0], cpu_fsr[0], src, 8, 8);
        break;
    case 0x06: /* FSR1L */
        tcg_gen_deposit_i32(cpu_fsr[1], cpu_fsr[1], src, 0, 8);
        break;
    case 0x07: /* FSR1H */
        tcg_gen_deposit_i32(cpu_fsr[1], cpu_fsr[1], src, 8, 8);
        break;
    case 0x08: /* BSR */
        tcg_gen_andi_i32(cpu_bsr, src, 0x3F);
        break;
    case 0x09: /* WREG */
        tcg_gen_mov_i32(cpu_wreg, src);
        break;
    case 0x0A: /* PCLATH */
        tcg_gen_andi_i32(cpu_pclath, src, 0x7F);
        break;
    case 0x0B: /* INTCON */
        tcg_gen_mov_i32(cpu_intcon, src);
        break;
    default:
        g_assert_not_reached();
    }
}

/* d = 0 stores to W, d = 1 stores back to f. */
static void gen_store_dest(DisasContext *ctx, TCGv val, int f, int d)
{
    if (d) {
        gen_store_f(ctx, val, f);
    } else {
        tcg_gen_mov_i32(cpu_wreg, val);
    }
}

/*
 * ALU
 */

static void gen_flag_z(TCGv res)
{
    tcg_gen_setcondi_i32(TCG_COND_EQ, cpu_Zf, res, 0);
}

/*
 * res = (a + b + cin) & 0xFF, setting C, DC and Z. Both inputs must already be
 * eight bits wide. Subtraction reaches here as a + ~b + 1, which is how the
 * hardware does it, so borrow comes out with the inverted sense the data sheet
 * describes (DS40002637A 9.7.4 note 1).
 */
static void gen_add8(TCGv res, TCGv a, TCGv b, TCGv cin)
{
    TCGv sum = tcg_temp_new_i32();
    TCGv lo = tcg_temp_new_i32();
    TCGv t = tcg_temp_new_i32();

    tcg_gen_andi_i32(lo, a, 0xF);
    tcg_gen_andi_i32(t, b, 0xF);
    tcg_gen_add_i32(lo, lo, t);
    tcg_gen_add_i32(lo, lo, cin);
    tcg_gen_extract_i32(cpu_DCf, lo, 4, 1);

    tcg_gen_add_i32(sum, a, b);
    tcg_gen_add_i32(sum, sum, cin);
    tcg_gen_extract_i32(cpu_Cf, sum, 8, 1);

    tcg_gen_andi_i32(res, sum, 0xFF);
    gen_flag_z(res);
}

static TCGv gen_not8(TCGv src)
{
    TCGv t = tcg_temp_new_i32();

    tcg_gen_xori_i32(t, src, 0xFF);
    return t;
}

/*
 * Byte-oriented file register operations
 */

static bool trans_ADDWF(DisasContext *ctx, arg_ADDWF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    gen_add8(val, val, cpu_wreg, tcg_constant_i32(0));
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_ADDWFC(DisasContext *ctx, arg_ADDWFC *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    gen_add8(val, val, cpu_wreg, cpu_Cf);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_SUBWF(DisasContext *ctx, arg_SUBWF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    gen_add8(val, val, gen_not8(cpu_wreg), tcg_constant_i32(1));
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_SUBWFB(DisasContext *ctx, arg_SUBWFB *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    gen_add8(val, val, gen_not8(cpu_wreg), cpu_Cf);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_ANDWF(DisasContext *ctx, arg_ANDWF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_and_i32(val, val, cpu_wreg);
    gen_flag_z(val);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_IORWF(DisasContext *ctx, arg_IORWF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_or_i32(val, val, cpu_wreg);
    gen_flag_z(val);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_XORWF(DisasContext *ctx, arg_XORWF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_xor_i32(val, val, cpu_wreg);
    gen_flag_z(val);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_COMF(DisasContext *ctx, arg_COMF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_xori_i32(val, val, 0xFF);
    gen_flag_z(val);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_DECF(DisasContext *ctx, arg_DECF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_subi_i32(val, val, 1);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_flag_z(val);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_INCF(DisasContext *ctx, arg_INCF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_addi_i32(val, val, 1);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_flag_z(val);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_MOVF(DisasContext *ctx, arg_MOVF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    gen_flag_z(val);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_SWAPF(DisasContext *ctx, arg_SWAPF *a)
{
    TCGv val = tcg_temp_new_i32();
    TCGv hi = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_shri_i32(hi, val, 4);
    tcg_gen_shli_i32(val, val, 4);
    tcg_gen_or_i32(val, val, hi);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_RLF(DisasContext *ctx, arg_RLF *a)
{
    TCGv val = tcg_temp_new_i32();
    TCGv carry = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_mov_i32(carry, cpu_Cf);
    tcg_gen_extract_i32(cpu_Cf, val, 7, 1);
    tcg_gen_shli_i32(val, val, 1);
    tcg_gen_or_i32(val, val, carry);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_RRF(DisasContext *ctx, arg_RRF *a)
{
    TCGv val = tcg_temp_new_i32();
    TCGv carry = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_shli_i32(carry, cpu_Cf, 7);
    tcg_gen_andi_i32(cpu_Cf, val, 1);
    tcg_gen_shri_i32(val, val, 1);
    tcg_gen_or_i32(val, val, carry);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_LSLF(DisasContext *ctx, arg_LSLF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_extract_i32(cpu_Cf, val, 7, 1);
    tcg_gen_shli_i32(val, val, 1);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_flag_z(val);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_LSRF(DisasContext *ctx, arg_LSRF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_andi_i32(cpu_Cf, val, 1);
    tcg_gen_shri_i32(val, val, 1);
    gen_flag_z(val);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_ASRF(DisasContext *ctx, arg_ASRF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_andi_i32(cpu_Cf, val, 1);
    /* Sign-extend from bit 7 before shifting, then mask back to eight bits. */
    tcg_gen_sextract_i32(val, val, 0, 8);
    tcg_gen_sari_i32(val, val, 1);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_flag_z(val);
    gen_store_dest(ctx, val, a->f, a->d);
    return true;
}

static bool trans_CLRF(DisasContext *ctx, arg_CLRF *a)
{
    gen_store_f(ctx, tcg_constant_i32(0), a->f);
    tcg_gen_movi_i32(cpu_Zf, 1);
    return true;
}

static bool trans_CLRW(DisasContext *ctx, arg_CLRW *a)
{
    tcg_gen_movi_i32(cpu_wreg, 0);
    tcg_gen_movi_i32(cpu_Zf, 1);
    return true;
}

static bool trans_MOVWF(DisasContext *ctx, arg_MOVWF *a)
{
    gen_store_f(ctx, cpu_wreg, a->f);
    return true;
}

/*
 * Byte-oriented skip operations
 */

static bool trans_DECFSZ(DisasContext *ctx, arg_DECFSZ *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_subi_i32(val, val, 1);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_store_dest(ctx, val, a->f, a->d);
    gen_skip_if(ctx, TCG_COND_EQ, val, 0);
    return true;
}

static bool trans_INCFSZ(DisasContext *ctx, arg_INCFSZ *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_addi_i32(val, val, 1);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_store_dest(ctx, val, a->f, a->d);
    gen_skip_if(ctx, TCG_COND_EQ, val, 0);
    return true;
}

/*
 * Bit-oriented file register operations
 */

static bool trans_BCF(DisasContext *ctx, arg_BCF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_andi_i32(val, val, ~(1 << a->b) & 0xFF);
    gen_store_f(ctx, val, a->f);
    return true;
}

static bool trans_BSF(DisasContext *ctx, arg_BSF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_ori_i32(val, val, 1 << a->b);
    gen_store_f(ctx, val, a->f);
    return true;
}

static bool trans_BTFSC(DisasContext *ctx, arg_BTFSC *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_andi_i32(val, val, 1 << a->b);
    gen_skip_if(ctx, TCG_COND_EQ, val, 0);
    return true;
}

static bool trans_BTFSS(DisasContext *ctx, arg_BTFSS *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f);
    tcg_gen_andi_i32(val, val, 1 << a->b);
    gen_skip_if(ctx, TCG_COND_NE, val, 0);
    return true;
}

/*
 * Literal operations
 */

static bool trans_ADDLW(DisasContext *ctx, arg_ADDLW *a)
{
    gen_add8(cpu_wreg, cpu_wreg, tcg_constant_i32(a->k),
             tcg_constant_i32(0));
    return true;
}

static bool trans_SUBLW(DisasContext *ctx, arg_SUBLW *a)
{
    /* k - W, computed as k + ~W + 1. */
    gen_add8(cpu_wreg, tcg_constant_i32(a->k), gen_not8(cpu_wreg),
             tcg_constant_i32(1));
    return true;
}

static bool trans_ANDLW(DisasContext *ctx, arg_ANDLW *a)
{
    tcg_gen_andi_i32(cpu_wreg, cpu_wreg, a->k);
    gen_flag_z(cpu_wreg);
    return true;
}

static bool trans_IORLW(DisasContext *ctx, arg_IORLW *a)
{
    tcg_gen_ori_i32(cpu_wreg, cpu_wreg, a->k);
    gen_flag_z(cpu_wreg);
    return true;
}

static bool trans_XORLW(DisasContext *ctx, arg_XORLW *a)
{
    tcg_gen_xori_i32(cpu_wreg, cpu_wreg, a->k);
    gen_flag_z(cpu_wreg);
    return true;
}

static bool trans_MOVLW(DisasContext *ctx, arg_MOVLW *a)
{
    tcg_gen_movi_i32(cpu_wreg, a->k);
    return true;
}

static bool trans_MOVLB(DisasContext *ctx, arg_MOVLB *a)
{
    tcg_gen_movi_i32(cpu_bsr, a->k);
    return true;
}

static bool trans_MOVLP(DisasContext *ctx, arg_MOVLP *a)
{
    tcg_gen_movi_i32(cpu_pclath, a->k);
    return true;
}

/*
 * Control operations
 *
 * GOTO and CALL supply PC[10:0]; PCLATH[6:3] supplies PC[14:11]. CALLW takes
 * PC[7:0] from W and PC[14:8] from PCLATH[6:0].
 */

static TCGv gen_pclath_target(uint32_t k11)
{
    TCGv dest = tcg_temp_new_i32();

    tcg_gen_andi_i32(dest, cpu_pclath, 0x78);
    tcg_gen_shli_i32(dest, dest, 8);
    tcg_gen_ori_i32(dest, dest, k11);
    return dest;
}

static bool trans_GOTO(DisasContext *ctx, arg_GOTO *a)
{
    gen_jump_indirect(ctx, gen_pclath_target(a->k));
    return true;
}

static bool trans_CALL(DisasContext *ctx, arg_CALL *a)
{
    TCGv dest = gen_pclath_target(a->k);

    gen_helper_push_stack(tcg_env, tcg_constant_i32(ctx->npc));
    gen_jump_indirect(ctx, dest);
    return true;
}

static bool trans_CALLW(DisasContext *ctx, arg_CALLW *a)
{
    TCGv dest = tcg_temp_new_i32();

    tcg_gen_andi_i32(dest, cpu_pclath, 0x7F);
    tcg_gen_shli_i32(dest, dest, 8);
    tcg_gen_or_i32(dest, dest, cpu_wreg);

    gen_helper_push_stack(tcg_env, tcg_constant_i32(ctx->npc));
    gen_jump_indirect(ctx, dest);
    return true;
}

static bool trans_BRA(DisasContext *ctx, arg_BRA *a)
{
    gen_jump_direct(ctx, (ctx->npc + a->k) & 0x7FFF);
    return true;
}

static bool trans_BRW(DisasContext *ctx, arg_BRW *a)
{
    TCGv dest = tcg_temp_new_i32();

    tcg_gen_addi_i32(dest, cpu_wreg, ctx->npc);
    gen_jump_indirect(ctx, dest);
    return true;
}

static bool trans_RETURN(DisasContext *ctx, arg_RETURN *a)
{
    TCGv dest = tcg_temp_new_i32();

    gen_helper_pop_stack(dest, tcg_env);
    gen_jump_indirect(ctx, dest);
    return true;
}

static bool trans_RETLW(DisasContext *ctx, arg_RETLW *a)
{
    TCGv dest = tcg_temp_new_i32();

    tcg_gen_movi_i32(cpu_wreg, a->k);
    gen_helper_pop_stack(dest, tcg_env);
    gen_jump_indirect(ctx, dest);
    return true;
}

static bool trans_RETFIE(DisasContext *ctx, arg_RETFIE *a)
{
    TCGv dest = tcg_temp_new_i32();

    gen_helper_retfie(dest, tcg_env);
    gen_jump_indirect(ctx, dest);
    return true;
}

/*
 * Inherent operations
 */

static bool trans_NOP(DisasContext *ctx, arg_NOP *a)
{
    return true;
}

static bool trans_CLRWDT(DisasContext *ctx, arg_CLRWDT *a)
{
    gen_helper_clrwdt(tcg_env);
    return true;
}

static bool trans_SLEEP(DisasContext *ctx, arg_SLEEP *a)
{
    tcg_gen_movi_i32(cpu_pc, ctx->npc);
    gen_helper_sleep(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_RESET(DisasContext *ctx, arg_RESET *a)
{
    gen_helper_reset(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_TRIS(DisasContext *ctx, arg_TRIS *a)
{
    gen_helper_tris(tcg_env, tcg_constant_i32(a->f));
    return true;
}

/*
 * C-compiler optimized operations
 */

static bool trans_ADDFSR(DisasContext *ctx, arg_ADDFSR *a)
{
    tcg_gen_addi_i32(cpu_fsr[a->n], cpu_fsr[a->n], a->k);
    tcg_gen_andi_i32(cpu_fsr[a->n], cpu_fsr[a->n], 0xFFFF);
    return true;
}

/*
 * mm selects the update applied to the FSR around the access:
 * 00 preincrement, 01 predecrement, 10 postincrement, 11 postdecrement.
 */
static void gen_fsr_adjust(int n, int mm, bool before)
{
    bool is_pre = (mm & 2) == 0;
    int delta = (mm & 1) ? -1 : 1;

    if (is_pre != before) {
        return;
    }
    tcg_gen_addi_i32(cpu_fsr[n], cpu_fsr[n], delta);
    tcg_gen_andi_i32(cpu_fsr[n], cpu_fsr[n], 0xFFFF);
}

static bool trans_MOVIW_mm(DisasContext *ctx, arg_MOVIW_mm *a)
{
    gen_fsr_adjust(a->n, a->mm, true);
    gen_helper_ld_data(cpu_wreg, tcg_env, cpu_fsr[a->n]);
    gen_fsr_adjust(a->n, a->mm, false);
    gen_flag_z(cpu_wreg);
    return true;
}

static bool trans_MOVWI_mm(DisasContext *ctx, arg_MOVWI_mm *a)
{
    gen_fsr_adjust(a->n, a->mm, true);
    gen_helper_st_data(tcg_env, cpu_fsr[a->n], cpu_wreg);
    gen_fsr_adjust(a->n, a->mm, false);
    return true;
}

static bool trans_MOVIW_k(DisasContext *ctx, arg_MOVIW_k *a)
{
    TCGv addr = tcg_temp_new_i32();

    tcg_gen_addi_i32(addr, cpu_fsr[a->n], a->k);
    tcg_gen_andi_i32(addr, addr, 0xFFFF);
    gen_helper_ld_data(cpu_wreg, tcg_env, addr);
    gen_flag_z(cpu_wreg);
    return true;
}

static bool trans_MOVWI_k(DisasContext *ctx, arg_MOVWI_k *a)
{
    TCGv addr = tcg_temp_new_i32();

    tcg_gen_addi_i32(addr, cpu_fsr[a->n], a->k);
    tcg_gen_andi_i32(addr, addr, 0xFFFF);
    gen_helper_st_data(tcg_env, addr, cpu_wreg);
    return true;
}

/*
 * Translator hooks
 */

static void translate(DisasContext *ctx)
{
    uint32_t opcode = translator_lduw(ctx->env, &ctx->base, ctx->npc * 2);

    ctx->npc++;

    if (!decode_insn(ctx, opcode)) {
        gen_helper_unsupported(tcg_env);
        ctx->base.is_jmp = DISAS_NORETURN;
    }
}

static void pic16_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    ctx->cs = cs;
    ctx->env = cpu_env(cs);
    ctx->npc = ctx->base.pc_first / 2;
}

static void pic16_tr_tb_start(DisasContextBase *db, CPUState *cs)
{
}

static void pic16_tr_insn_start(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    tcg_gen_insn_start(ctx->npc, 0, 0);
}

static void pic16_tr_translate_insn(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    translate(ctx);

    ctx->base.pc_next = ctx->npc * 2;

    if (ctx->base.is_jmp == DISAS_NEXT) {
        target_ulong page_first = ctx->base.pc_first & TARGET_PAGE_MASK;

        if ((ctx->base.pc_next - page_first) >= TARGET_PAGE_SIZE - 4) {
            ctx->base.is_jmp = DISAS_TOO_MANY;
        }
    }
}

static void pic16_tr_tb_stop(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    switch (ctx->base.is_jmp) {
    case DISAS_NORETURN:
        break;
    case DISAS_NEXT:
    case DISAS_TOO_MANY:
        gen_goto_tb(ctx, 1, ctx->npc);
        break;
    default:
        g_assert_not_reached();
    }
}

static const TranslatorOps pic16_tr_ops = {
    .init_disas_context = pic16_tr_init_disas_context,
    .tb_start           = pic16_tr_tb_start,
    .insn_start         = pic16_tr_insn_start,
    .translate_insn     = pic16_tr_translate_insn,
    .tb_stop            = pic16_tr_tb_stop,
};

void pic16_cpu_translate_code(CPUState *cs, TranslationBlock *tb,
                              int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext dc = { };
    translator_loop(cs, tb, max_insns, pc, host_pc, &pic16_tr_ops, &dc.base,
                    TCG_TYPE_VA);
}

void pic16_cpu_tcg_init(void)
{
    cpu_pc = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(pc_w), "pc");

    cpu_wreg = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(wreg), "W");
    cpu_bsr = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(bsr), "BSR");
    cpu_pclath = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(pclath),
                                        "PCLATH");
    cpu_intcon = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(intcon),
                                        "INTCON");
    cpu_fsr[0] = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(fsr[0]),
                                        "FSR0");
    cpu_fsr[1] = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(fsr[1]),
                                        "FSR1");

    cpu_Cf = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(sregC), "Cf");
    cpu_DCf = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(sregDC), "DCf");
    cpu_Zf = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(sregZ), "Zf");
    cpu_PDf = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(sregPD), "PDf");
    cpu_TOf = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(sregTO), "TOf");
}
