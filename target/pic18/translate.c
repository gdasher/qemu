/*
 * PIC18 (Q43 core, standard instruction set) translation
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
static TCGv cpu_pclatu;
static TCGv cpu_intcon0;
static TCGv cpu_fsr[3];
static TCGv cpu_prodl;
static TCGv cpu_prodh;
static TCGv cpu_tablat;
static TCGv cpu_tblptr;

static TCGv cpu_Cf;
static TCGv cpu_DCf;
static TCGv cpu_Zf;
static TCGv cpu_OVf;
static TCGv cpu_Nf;
static TCGv cpu_PDf;
static TCGv cpu_TOf;

#define PIC18_REG_OFFS(x) offsetof(CPUPIC18State, x)

typedef struct DisasContext {
    DisasContextBase base;

    CPUPIC18State *env;
    CPUState *cs;

    /* Byte address of the instruction (word) after the one being decoded. */
    uint32_t npc;

    /*
     * Set when the instruction changed something the interrupt logic depends
     * on (GIE, IPEN). The block then ends with an exit to the main loop rather
     * than a chained jump, so a newly enabled request is taken at once.
     */
    bool need_exit;

    /*
     * FSR updates owed by the indirect operands of the current instruction.
     * They are applied after its last memory access (see gen_flush_fsr), so
     * that an access which turns out to be MMIO -- and makes QEMU restart the
     * instruction on its own -- does not step the FSR twice.
     */
    struct {
        int n;
        int mode;
        TCGv addr;
    } pend[2];
    int npend;
} DisasContext;

static void gen_flush_fsr(DisasContext *ctx);

/* Include the auto-generated decoder. */
static bool decode_insn(DisasContext *ctx, uint16_t insn);
#include "decode-insn.c.inc"

/*
 * Multi-word instructions: fetch the continuation word. The hardware only
 * looks at the low twelve bits; the top nibble is 1111 so the word is a NOP
 * if it is ever executed on its own.
 */
static uint32_t next_word(DisasContext *ctx)
{
    uint32_t word = translator_lduw(ctx->env, &ctx->base, ctx->npc);

    ctx->npc += 2;
    return word;
}

/*
 * Control flow
 */

static void gen_goto_tb(DisasContext *ctx, int n, uint32_t dest)
{
    dest &= PIC18_PC_MASK;

    if (ctx->need_exit) {
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_exit_tb(NULL, 0);
    } else if (translator_use_goto_tb(&ctx->base, dest)) {
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
    gen_flush_fsr(ctx);
    gen_goto_tb(ctx, 0, dest);
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_jump_indirect(DisasContext *ctx, TCGv dest)
{
    gen_flush_fsr(ctx);
    tcg_gen_andi_i32(cpu_pc, dest, PIC18_PC_MASK);
    if (ctx->need_exit) {
        tcg_gen_exit_tb(NULL, 0);
    } else {
        tcg_gen_lookup_and_goto_ptr();
    }
    ctx->base.is_jmp = DISAS_NORETURN;
}

/* Ends the block through the main loop, so pending interrupts are seen. */
static void gen_jump_exit(DisasContext *ctx, TCGv dest)
{
    gen_flush_fsr(ctx);
    tcg_gen_andi_i32(cpu_pc, dest, PIC18_PC_MASK);
    tcg_gen_exit_tb(NULL, 0);
    ctx->base.is_jmp = DISAS_NORETURN;
}

/*
 * A skip is a conditional jump over the following word. If that word is the
 * first of a multi-word instruction, the continuation words execute as NOPs,
 * which is what the hardware does too. Ending the translation block here
 * costs some chaining, but it keeps the skip entirely within one instruction
 * instead of carrying state into the next.
 */
static void gen_skip_if(DisasContext *ctx, TCGCond cond, TCGv val, int cmp)
{
    TCGLabel *no_skip = gen_new_label();

    gen_flush_fsr(ctx);
    tcg_gen_brcondi_i32(tcg_invert_cond(cond), val, cmp, no_skip);
    gen_goto_tb(ctx, 0, ctx->npc + 2);
    gen_set_label(no_skip);
    gen_goto_tb(ctx, 1, ctx->npc);
    ctx->base.is_jmp = DISAS_NORETURN;
}

/* Conditional branch on one flag: taken when flag == val. */
static void gen_branch_if(DisasContext *ctx, TCGv flag, int val, uint32_t dest)
{
    TCGLabel *not_taken = gen_new_label();

    gen_flush_fsr(ctx);
    tcg_gen_brcondi_i32(TCG_COND_NE, flag, val, not_taken);
    gen_goto_tb(ctx, 0, dest);
    gen_set_label(not_taken);
    gen_goto_tb(ctx, 1, ctx->npc);
    ctx->base.is_jmp = DISAS_NORETURN;
}

/*
 * STATUS
 */

static void gen_status_read(TCGv dest)
{
    TCGv t = tcg_temp_new_i32();

    tcg_gen_shli_i32(dest, cpu_TOf, PIC18_STATUS_TO);
    tcg_gen_shli_i32(t, cpu_PDf, PIC18_STATUS_PD);
    tcg_gen_or_i32(dest, dest, t);
    tcg_gen_shli_i32(t, cpu_Nf, PIC18_STATUS_N);
    tcg_gen_or_i32(dest, dest, t);
    tcg_gen_shli_i32(t, cpu_OVf, PIC18_STATUS_OV);
    tcg_gen_or_i32(dest, dest, t);
    tcg_gen_shli_i32(t, cpu_Zf, PIC18_STATUS_Z);
    tcg_gen_or_i32(dest, dest, t);
    tcg_gen_shli_i32(t, cpu_DCf, PIC18_STATUS_DC);
    tcg_gen_or_i32(dest, dest, t);
    tcg_gen_or_i32(dest, dest, cpu_Cf);
}

/* TO and PD are read-only. */
static void gen_status_write(TCGv src)
{
    tcg_gen_extract_i32(cpu_Nf, src, PIC18_STATUS_N, 1);
    tcg_gen_extract_i32(cpu_OVf, src, PIC18_STATUS_OV, 1);
    tcg_gen_extract_i32(cpu_Zf, src, PIC18_STATUS_Z, 1);
    tcg_gen_extract_i32(cpu_DCf, src, PIC18_STATUS_DC, 1);
    tcg_gen_andi_i32(cpu_Cf, src, 1);
}

/*
 * Indirect access through an FSR.
 *
 * The five indirect operands of each FSR are virtual registers: INDFn reads
 * or writes the byte the FSR points at; POSTINCn/POSTDECn do the same and
 * then step the FSR; PREINCn steps first; PLUSWn adds W as a signed offset
 * without changing the FSR. The target may be anything in data space,
 * including the core registers, so the access goes through a helper that
 * resolves it at run time. The PC is committed first so the helper can serve
 * a read of PCL.
 */
/* The effective address; the FSR itself is not touched here. */
static void gen_indirect_addr(DisasContext *ctx, TCGv addr, int n, int mode)
{
    switch (mode) {
    case PIC18_IND_PREINC:
        tcg_gen_addi_i32(addr, cpu_fsr[n], 1);
        tcg_gen_andi_i32(addr, addr, PIC18_FSR_MASK);
        break;
    case PIC18_IND_PLUSW:
        tcg_gen_sextract_i32(addr, cpu_wreg, 0, 8);
        tcg_gen_add_i32(addr, addr, cpu_fsr[n]);
        tcg_gen_andi_i32(addr, addr, PIC18_FSR_MASK);
        break;
    default:
        tcg_gen_mov_i32(addr, cpu_fsr[n]);
        break;
    }
}

/*
 * The FSR update an indirect operand owes. It is queued rather than done,
 * because an instruction must not change guest state before its last memory
 * access: if that access is MMIO, QEMU restarts the instruction in a block
 * of its own (cpu_io_recompile), and anything already committed would be
 * done twice. A read-modify-write names the same operand twice and owes one
 * update, hence the duplicate check.
 */
static void gen_pend_fsr(DisasContext *ctx, int n, int mode, TCGv addr)
{
    int i;

    if (mode == PIC18_IND_INDF || mode == PIC18_IND_PLUSW) {
        return;
    }
    for (i = 0; i < ctx->npend; i++) {
        if (ctx->pend[i].n == n && ctx->pend[i].mode == mode) {
            return;
        }
    }
    g_assert(ctx->npend < ARRAY_SIZE(ctx->pend));
    ctx->pend[ctx->npend].n = n;
    ctx->pend[ctx->npend].mode = mode;
    ctx->pend[ctx->npend].addr = addr;
    ctx->npend++;
}

static void gen_flush_fsr(DisasContext *ctx)
{
    int i;

    for (i = 0; i < ctx->npend; i++) {
        int n = ctx->pend[i].n;

        switch (ctx->pend[i].mode) {
        case PIC18_IND_PREINC:
            tcg_gen_mov_i32(cpu_fsr[n], ctx->pend[i].addr);
            break;
        case PIC18_IND_POSTINC:
            tcg_gen_addi_i32(cpu_fsr[n], cpu_fsr[n], 1);
            tcg_gen_andi_i32(cpu_fsr[n], cpu_fsr[n], PIC18_FSR_MASK);
            break;
        case PIC18_IND_POSTDEC:
            tcg_gen_subi_i32(cpu_fsr[n], cpu_fsr[n], 1);
            tcg_gen_andi_i32(cpu_fsr[n], cpu_fsr[n], PIC18_FSR_MASK);
            break;
        default:
            g_assert_not_reached();
        }
    }
    ctx->npend = 0;
}

static void gen_indirect_load(DisasContext *ctx, TCGv dest, int n, int mode)
{
    TCGv addr = tcg_temp_new_i32();

    gen_indirect_addr(ctx, addr, n, mode);
    tcg_gen_movi_i32(cpu_pc, ctx->npc);
    gen_helper_ld_data(dest, tcg_env, addr);
    gen_pend_fsr(ctx, n, mode, addr);
}

static void gen_indirect_store(DisasContext *ctx, TCGv src, int n, int mode)
{
    TCGv addr = tcg_temp_new_i32();

    gen_indirect_addr(ctx, addr, n, mode);
    tcg_gen_movi_i32(cpu_pc, ctx->npc);
    gen_helper_st_data(tcg_env, addr, src);
    gen_pend_fsr(ctx, n, mode, addr);
}

/*
 * Core registers, 0x4D6-0x4FF. These live in CPUPIC18State and are resolved
 * here when the address is known at translation time. The ones that are
 * awkward or rare (stack, PCON, INTCON1) go through a helper.
 */

static bool is_core_reg(uint32_t addr)
{
    return addr >= PIC18_CORE_SFR_BASE && addr < PIC18_SFR_END;
}

/* The FSR number and mode of an indirect operand address, or -1. */
static int indirect_of(uint32_t addr, int *mode)
{
    switch (addr) {
    case PIC18_REG_PLUSW2 ... PIC18_REG_INDF2:
        *mode = addr - PIC18_REG_PLUSW2;
        return 2;
    case PIC18_REG_PLUSW1 ... PIC18_REG_INDF1:
        *mode = addr - PIC18_REG_PLUSW1;
        return 1;
    case PIC18_REG_PLUSW0 ... PIC18_REG_INDF0:
        *mode = addr - PIC18_REG_PLUSW0;
        return 0;
    default:
        return -1;
    }
}

static void gen_load_core(DisasContext *ctx, TCGv dest, uint32_t addr)
{
    int mode;
    int n = indirect_of(addr, &mode);

    if (n >= 0) {
        gen_indirect_load(ctx, dest, n, mode);
        return;
    }

    switch (addr) {
    case PIC18_REG_INTCON0:
        tcg_gen_mov_i32(dest, cpu_intcon0);
        break;
    case PIC18_REG_STATUS:
        gen_status_read(dest);
        break;
    case PIC18_REG_FSR2L:
        tcg_gen_andi_i32(dest, cpu_fsr[2], 0xFF);
        break;
    case PIC18_REG_FSR2H:
        tcg_gen_extract_i32(dest, cpu_fsr[2], 8, 8);
        break;
    case PIC18_REG_BSR:
        tcg_gen_mov_i32(dest, cpu_bsr);
        break;
    case PIC18_REG_FSR1L:
        tcg_gen_andi_i32(dest, cpu_fsr[1], 0xFF);
        break;
    case PIC18_REG_FSR1H:
        tcg_gen_extract_i32(dest, cpu_fsr[1], 8, 8);
        break;
    case PIC18_REG_WREG:
        tcg_gen_mov_i32(dest, cpu_wreg);
        break;
    case PIC18_REG_FSR0L:
        tcg_gen_andi_i32(dest, cpu_fsr[0], 0xFF);
        break;
    case PIC18_REG_FSR0H:
        tcg_gen_extract_i32(dest, cpu_fsr[0], 8, 8);
        break;
    case PIC18_REG_PRODL:
        tcg_gen_mov_i32(dest, cpu_prodl);
        break;
    case PIC18_REG_PRODH:
        tcg_gen_mov_i32(dest, cpu_prodh);
        break;
    case PIC18_REG_TABLAT:
        tcg_gen_mov_i32(dest, cpu_tablat);
        break;
    case PIC18_REG_TBLPTRL:
        tcg_gen_andi_i32(dest, cpu_tblptr, 0xFF);
        break;
    case PIC18_REG_TBLPTRH:
        tcg_gen_extract_i32(dest, cpu_tblptr, 8, 8);
        break;
    case PIC18_REG_TBLPTRU:
        tcg_gen_extract_i32(dest, cpu_tblptr, 16, 8);
        break;
    case PIC18_REG_PCL:
        /*
         * The PC has already advanced past this instruction, and a read of
         * PCL latches the upper bytes into PCLATH and PCLATU
         * (DS40002147F 9.1.2).
         */
        tcg_gen_movi_i32(dest, ctx->npc & 0xFF);
        tcg_gen_movi_i32(cpu_pclath, (ctx->npc >> 8) & 0xFF);
        tcg_gen_movi_i32(cpu_pclatu, (ctx->npc >> 16) & 0x1F);
        break;
    case PIC18_REG_PCLATH:
        tcg_gen_mov_i32(dest, cpu_pclath);
        break;
    case PIC18_REG_PCLATU:
        tcg_gen_mov_i32(dest, cpu_pclatu);
        break;
    default:
        /* INTCON1, PCON0, PCON1, CPUDOZE, STKPTR, TOSL/H/U */
        gen_helper_ld_core(dest, tcg_env, tcg_constant_i32(addr));
        break;
    }
}

static void gen_store_core(DisasContext *ctx, TCGv src, uint32_t addr)
{
    int mode;
    int n = indirect_of(addr, &mode);

    if (n >= 0) {
        gen_indirect_store(ctx, src, n, mode);
        return;
    }

    switch (addr) {
    case PIC18_REG_INTCON0:
        tcg_gen_mov_i32(cpu_intcon0, src);
        ctx->need_exit = true;
        break;
    case PIC18_REG_STATUS:
        gen_status_write(src);
        break;
    case PIC18_REG_FSR2L:
        tcg_gen_deposit_i32(cpu_fsr[2], cpu_fsr[2], src, 0, 8);
        break;
    case PIC18_REG_FSR2H:
        tcg_gen_deposit_i32(cpu_fsr[2], cpu_fsr[2], src, 8, 6);
        break;
    case PIC18_REG_BSR:
        tcg_gen_andi_i32(cpu_bsr, src, 0x3F);
        break;
    case PIC18_REG_FSR1L:
        tcg_gen_deposit_i32(cpu_fsr[1], cpu_fsr[1], src, 0, 8);
        break;
    case PIC18_REG_FSR1H:
        tcg_gen_deposit_i32(cpu_fsr[1], cpu_fsr[1], src, 8, 6);
        break;
    case PIC18_REG_WREG:
        tcg_gen_mov_i32(cpu_wreg, src);
        break;
    case PIC18_REG_FSR0L:
        tcg_gen_deposit_i32(cpu_fsr[0], cpu_fsr[0], src, 0, 8);
        break;
    case PIC18_REG_FSR0H:
        tcg_gen_deposit_i32(cpu_fsr[0], cpu_fsr[0], src, 8, 6);
        break;
    case PIC18_REG_PRODL:
        tcg_gen_mov_i32(cpu_prodl, src);
        break;
    case PIC18_REG_PRODH:
        tcg_gen_mov_i32(cpu_prodh, src);
        break;
    case PIC18_REG_TABLAT:
        tcg_gen_mov_i32(cpu_tablat, src);
        break;
    case PIC18_REG_TBLPTRL:
        tcg_gen_deposit_i32(cpu_tblptr, cpu_tblptr, src, 0, 8);
        break;
    case PIC18_REG_TBLPTRH:
        tcg_gen_deposit_i32(cpu_tblptr, cpu_tblptr, src, 8, 8);
        break;
    case PIC18_REG_TBLPTRU:
        tcg_gen_deposit_i32(cpu_tblptr, cpu_tblptr, src, 16, 6);
        break;
    case PIC18_REG_PCL:
    {
        /* A write to PCL is a computed jump, high bits from PCLATU:PCLATH. */
        TCGv dest = tcg_temp_new_i32();

        tcg_gen_shli_i32(dest, cpu_pclatu, 16);
        tcg_gen_deposit_i32(dest, dest, cpu_pclath, 8, 8);
        tcg_gen_deposit_i32(dest, dest, src, 0, 8);
        gen_jump_indirect(ctx, dest);
        break;
    }
    case PIC18_REG_PCLATH:
        tcg_gen_mov_i32(cpu_pclath, src);
        break;
    case PIC18_REG_PCLATU:
        tcg_gen_andi_i32(cpu_pclatu, src, 0x1F);
        break;
    default:
        /* INTCON1, PCON0, PCON1, CPUDOZE, STKPTR, TOSL/H/U */
        gen_helper_st_core(tcg_env, tcg_constant_i32(addr), src);
        break;
    }
}

/*
 * Data memory at a constant 14-bit address (MOVFF, MOVFFL, and the access
 * bank). Core registers resolve above; everything else is a memory access
 * that the SoC's RAM and devices serve.
 */
static void gen_load_abs(DisasContext *ctx, TCGv dest, uint32_t addr)
{
    if (is_core_reg(addr)) {
        gen_load_core(ctx, dest, addr);
    } else {
        tcg_gen_qemu_ld_tl(dest, tcg_constant_i32(addr), MMU_DATA_IDX, MO_UB);
    }
}

static void gen_store_abs(DisasContext *ctx, TCGv src, uint32_t addr)
{
    if (is_core_reg(addr)) {
        gen_store_core(ctx, src, addr);
    } else {
        tcg_gen_qemu_st_tl(src, tcg_constant_i32(addr), MMU_DATA_IDX, MO_UB);
    }
}

/*
 * Banked access (a = 1): the address is (BSR << 8) | f and is only known at
 * run time. It lands on a core register exactly when BSR is 4 and f is at
 * least 0xD6, so only those f need a run-time check; the rest go straight to
 * memory.
 */
static TCGv gen_banked_addr(int f)
{
    TCGv addr = tcg_temp_new_i32();

    tcg_gen_shli_i32(addr, cpu_bsr, 8);
    tcg_gen_ori_i32(addr, addr, f);
    return addr;
}

static void gen_load_banked(DisasContext *ctx, TCGv dest, int f)
{
    TCGv addr = gen_banked_addr(f);

    if (f >= (PIC18_CORE_SFR_BASE & 0xFF)) {
        TCGLabel *core = gen_new_label();
        TCGLabel *done = gen_new_label();

        tcg_gen_brcondi_i32(TCG_COND_EQ, cpu_bsr, PIC18_CORE_SFR_BASE >> 8,
                            core);
        tcg_gen_qemu_ld_tl(dest, addr, MMU_DATA_IDX, MO_UB);
        tcg_gen_br(done);
        gen_set_label(core);
        tcg_gen_movi_i32(cpu_pc, ctx->npc);
        gen_helper_ld_banked(dest, tcg_env, addr);
        gen_set_label(done);
    } else {
        tcg_gen_qemu_ld_tl(dest, addr, MMU_DATA_IDX, MO_UB);
    }
}

static void gen_store_banked(DisasContext *ctx, TCGv src, int f)
{
    TCGv addr = gen_banked_addr(f);

    if (f >= (PIC18_CORE_SFR_BASE & 0xFF)) {
        TCGLabel *core = gen_new_label();
        TCGLabel *done = gen_new_label();

        tcg_gen_brcondi_i32(TCG_COND_EQ, cpu_bsr, PIC18_CORE_SFR_BASE >> 8,
                            core);
        tcg_gen_qemu_st_tl(src, addr, MMU_DATA_IDX, MO_UB);
        tcg_gen_br(done);
        gen_set_label(core);
        tcg_gen_movi_i32(cpu_pc, ctx->npc);
        gen_helper_st_banked(tcg_env, addr, src);
        gen_set_label(done);
    } else {
        tcg_gen_qemu_st_tl(src, addr, MMU_DATA_IDX, MO_UB);
    }
}

/*
 * File register access with the a bit. a = 0 is the access bank, whose
 * address is a constant; a = 1 is banked through BSR.
 */
static uint32_t access_bank_addr(int f)
{
    if (f < PIC18_ACCESS_SPLIT) {
        return PIC18_ACCESS_RAM_BASE + f;
    }
    return PIC18_ACCESS_SFR_BASE + f;
}

static void gen_load_f(DisasContext *ctx, TCGv dest, int f, int a)
{
    if (a) {
        gen_load_banked(ctx, dest, f);
    } else {
        gen_load_abs(ctx, dest, access_bank_addr(f));
    }
}

static void gen_store_f(DisasContext *ctx, TCGv src, int f, int a)
{
    if (a) {
        gen_store_banked(ctx, src, f);
    } else {
        gen_store_abs(ctx, src, access_bank_addr(f));
    }
}

/* d = 0 stores to W, d = 1 stores back to f. */
static void gen_store_dest(DisasContext *ctx, TCGv val, int f, int d, int a)
{
    if (d) {
        gen_store_f(ctx, val, f, a);
    } else {
        tcg_gen_mov_i32(cpu_wreg, val);
    }
}

/*
 * ALU
 */

static void gen_flag_zn(TCGv res)
{
    tcg_gen_setcondi_i32(TCG_COND_EQ, cpu_Zf, res, 0);
    tcg_gen_extract_i32(cpu_Nf, res, 7, 1);
}

/*
 * res = (a + b + cin) & 0xFF, setting C, DC, Z, OV and N. Both inputs must
 * already be eight bits wide. Subtraction reaches here as a + ~b + 1, which is
 * how the hardware does it, so borrow comes out with the inverted sense the
 * data sheet describes, and the signed overflow test on the addition is the
 * right one for the subtraction as well.
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

    tcg_gen_andi_i32(sum, sum, 0xFF);

    /* Overflow: both inputs have the same sign and the result does not. */
    tcg_gen_xor_i32(t, a, sum);
    tcg_gen_xor_i32(lo, b, sum);
    tcg_gen_and_i32(t, t, lo);
    tcg_gen_extract_i32(cpu_OVf, t, 7, 1);

    tcg_gen_mov_i32(res, sum);
    gen_flag_zn(res);
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

    gen_load_f(ctx, val, a->f, a->a);
    gen_add8(val, val, cpu_wreg, tcg_constant_i32(0));
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_ADDWFC(DisasContext *ctx, arg_ADDWFC *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    gen_add8(val, val, cpu_wreg, cpu_Cf);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_ANDWF(DisasContext *ctx, arg_ANDWF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_and_i32(val, val, cpu_wreg);
    gen_flag_zn(val);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_CLRF(DisasContext *ctx, arg_CLRF *a)
{
    gen_store_f(ctx, tcg_constant_i32(0), a->f, a->a);
    tcg_gen_movi_i32(cpu_Zf, 1);
    return true;
}

static bool trans_COMF(DisasContext *ctx, arg_COMF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_xori_i32(val, val, 0xFF);
    gen_flag_zn(val);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_DECF(DisasContext *ctx, arg_DECF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    /* f - 1 as f + 0xFF: C is set unless f was zero. */
    gen_add8(val, val, tcg_constant_i32(0xFF), tcg_constant_i32(0));
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_INCF(DisasContext *ctx, arg_INCF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    gen_add8(val, val, tcg_constant_i32(1), tcg_constant_i32(0));
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_IORWF(DisasContext *ctx, arg_IORWF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_or_i32(val, val, cpu_wreg);
    gen_flag_zn(val);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_MOVF(DisasContext *ctx, arg_MOVF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    gen_flag_zn(val);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_MOVFF(DisasContext *ctx, arg_MOVFF *a)
{
    TCGv val = tcg_temp_new_i32();
    uint32_t dst = next_word(ctx) & 0xFFF;

    gen_load_abs(ctx, val, a->k);
    gen_store_abs(ctx, val, dst);
    return true;
}

static bool trans_MOVFFL(DisasContext *ctx, arg_MOVFFL *a)
{
    TCGv val = tcg_temp_new_i32();
    uint32_t w2 = next_word(ctx);
    uint32_t w3 = next_word(ctx);
    /* fs<13:10> in the first word, fs<9:0> and fd<13:12> in the second. */
    uint32_t src = (a->k << 10) | ((w2 >> 2) & 0x3FF);
    uint32_t dst = ((w2 & 3) << 12) | (w3 & 0xFFF);

    gen_load_abs(ctx, val, src);
    gen_store_abs(ctx, val, dst);
    return true;
}

static bool trans_MOVWF(DisasContext *ctx, arg_MOVWF *a)
{
    gen_store_f(ctx, cpu_wreg, a->f, a->a);
    return true;
}

static bool trans_MULWF(DisasContext *ctx, arg_MULWF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_mul_i32(val, val, cpu_wreg);
    tcg_gen_andi_i32(cpu_prodl, val, 0xFF);
    tcg_gen_extract_i32(cpu_prodh, val, 8, 8);
    return true;
}

static bool trans_NEGF(DisasContext *ctx, arg_NEGF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    /* 0 - f as 0 + ~f + 1. */
    gen_add8(val, tcg_constant_i32(0), gen_not8(val), tcg_constant_i32(1));
    gen_store_f(ctx, val, a->f, a->a);
    return true;
}

static bool trans_RLCF(DisasContext *ctx, arg_RLCF *a)
{
    TCGv val = tcg_temp_new_i32();
    TCGv carry = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_mov_i32(carry, cpu_Cf);
    tcg_gen_extract_i32(cpu_Cf, val, 7, 1);
    tcg_gen_shli_i32(val, val, 1);
    tcg_gen_or_i32(val, val, carry);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_flag_zn(val);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_RLNCF(DisasContext *ctx, arg_RLNCF *a)
{
    TCGv val = tcg_temp_new_i32();
    TCGv hi = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_shri_i32(hi, val, 7);
    tcg_gen_shli_i32(val, val, 1);
    tcg_gen_or_i32(val, val, hi);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_flag_zn(val);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_RRCF(DisasContext *ctx, arg_RRCF *a)
{
    TCGv val = tcg_temp_new_i32();
    TCGv carry = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_shli_i32(carry, cpu_Cf, 7);
    tcg_gen_andi_i32(cpu_Cf, val, 1);
    tcg_gen_shri_i32(val, val, 1);
    tcg_gen_or_i32(val, val, carry);
    gen_flag_zn(val);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_RRNCF(DisasContext *ctx, arg_RRNCF *a)
{
    TCGv val = tcg_temp_new_i32();
    TCGv lo = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_shli_i32(lo, val, 7);
    tcg_gen_shri_i32(val, val, 1);
    tcg_gen_or_i32(val, val, lo);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_flag_zn(val);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_SETF(DisasContext *ctx, arg_SETF *a)
{
    gen_store_f(ctx, tcg_constant_i32(0xFF), a->f, a->a);
    return true;
}

static bool trans_SUBFWB(DisasContext *ctx, arg_SUBFWB *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    /* W - f - !C as W + ~f + C. */
    gen_add8(val, cpu_wreg, gen_not8(val), cpu_Cf);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_SUBWF(DisasContext *ctx, arg_SUBWF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    gen_add8(val, val, gen_not8(cpu_wreg), tcg_constant_i32(1));
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_SUBWFB(DisasContext *ctx, arg_SUBWFB *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    /* f - W - !C as f + ~W + C. */
    gen_add8(val, val, gen_not8(cpu_wreg), cpu_Cf);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_SWAPF(DisasContext *ctx, arg_SWAPF *a)
{
    TCGv val = tcg_temp_new_i32();
    TCGv hi = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_shri_i32(hi, val, 4);
    tcg_gen_shli_i32(val, val, 4);
    tcg_gen_or_i32(val, val, hi);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

static bool trans_XORWF(DisasContext *ctx, arg_XORWF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_xor_i32(val, val, cpu_wreg);
    gen_flag_zn(val);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    return true;
}

/*
 * Byte-oriented skip operations
 */

static bool trans_CPFSEQ(DisasContext *ctx, arg_CPFSEQ *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_sub_i32(val, val, cpu_wreg);
    gen_skip_if(ctx, TCG_COND_EQ, val, 0);
    return true;
}

static bool trans_CPFSGT(DisasContext *ctx, arg_CPFSGT *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    /* Both are unsigned bytes, so the difference is negative iff f <= W. */
    tcg_gen_sub_i32(val, val, cpu_wreg);
    gen_skip_if(ctx, TCG_COND_GT, val, 0);
    return true;
}

static bool trans_CPFSLT(DisasContext *ctx, arg_CPFSLT *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_sub_i32(val, val, cpu_wreg);
    gen_skip_if(ctx, TCG_COND_LT, val, 0);
    return true;
}

static bool trans_DECFSZ(DisasContext *ctx, arg_DECFSZ *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_subi_i32(val, val, 1);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    gen_skip_if(ctx, TCG_COND_EQ, val, 0);
    return true;
}

static bool trans_DCFSNZ(DisasContext *ctx, arg_DCFSNZ *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_subi_i32(val, val, 1);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    gen_skip_if(ctx, TCG_COND_NE, val, 0);
    return true;
}

static bool trans_INCFSZ(DisasContext *ctx, arg_INCFSZ *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_addi_i32(val, val, 1);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    gen_skip_if(ctx, TCG_COND_EQ, val, 0);
    return true;
}

static bool trans_INFSNZ(DisasContext *ctx, arg_INFSNZ *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_addi_i32(val, val, 1);
    tcg_gen_andi_i32(val, val, 0xFF);
    gen_store_dest(ctx, val, a->f, a->d, a->a);
    gen_skip_if(ctx, TCG_COND_NE, val, 0);
    return true;
}

static bool trans_TSTFSZ(DisasContext *ctx, arg_TSTFSZ *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    gen_skip_if(ctx, TCG_COND_EQ, val, 0);
    return true;
}

/*
 * Bit-oriented file register operations
 */

static bool trans_BCF(DisasContext *ctx, arg_BCF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_andi_i32(val, val, ~(1 << a->b) & 0xFF);
    gen_store_f(ctx, val, a->f, a->a);
    return true;
}

static bool trans_BSF(DisasContext *ctx, arg_BSF *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_ori_i32(val, val, 1 << a->b);
    gen_store_f(ctx, val, a->f, a->a);
    return true;
}

static bool trans_BTG(DisasContext *ctx, arg_BTG *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_xori_i32(val, val, 1 << a->b);
    gen_store_f(ctx, val, a->f, a->a);
    return true;
}

static bool trans_BTFSC(DisasContext *ctx, arg_BTFSC *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_andi_i32(val, val, 1 << a->b);
    gen_skip_if(ctx, TCG_COND_EQ, val, 0);
    return true;
}

static bool trans_BTFSS(DisasContext *ctx, arg_BTFSS *a)
{
    TCGv val = tcg_temp_new_i32();

    gen_load_f(ctx, val, a->f, a->a);
    tcg_gen_andi_i32(val, val, 1 << a->b);
    gen_skip_if(ctx, TCG_COND_NE, val, 0);
    return true;
}

/*
 * Control operations. Relative offsets count words from the instruction
 * after the branch.
 */

static uint32_t rel_target(DisasContext *ctx, int n)
{
    return (ctx->npc + 2 * n) & PIC18_PC_MASK;
}

static bool trans_BC(DisasContext *ctx, arg_BC *a)
{
    gen_branch_if(ctx, cpu_Cf, 1, rel_target(ctx, a->n));
    return true;
}

static bool trans_BN(DisasContext *ctx, arg_BN *a)
{
    gen_branch_if(ctx, cpu_Nf, 1, rel_target(ctx, a->n));
    return true;
}

static bool trans_BNC(DisasContext *ctx, arg_BNC *a)
{
    gen_branch_if(ctx, cpu_Cf, 0, rel_target(ctx, a->n));
    return true;
}

static bool trans_BNN(DisasContext *ctx, arg_BNN *a)
{
    gen_branch_if(ctx, cpu_Nf, 0, rel_target(ctx, a->n));
    return true;
}

static bool trans_BNOV(DisasContext *ctx, arg_BNOV *a)
{
    gen_branch_if(ctx, cpu_OVf, 0, rel_target(ctx, a->n));
    return true;
}

static bool trans_BNZ(DisasContext *ctx, arg_BNZ *a)
{
    gen_branch_if(ctx, cpu_Zf, 0, rel_target(ctx, a->n));
    return true;
}

static bool trans_BOV(DisasContext *ctx, arg_BOV *a)
{
    gen_branch_if(ctx, cpu_OVf, 1, rel_target(ctx, a->n));
    return true;
}

static bool trans_BZ(DisasContext *ctx, arg_BZ *a)
{
    gen_branch_if(ctx, cpu_Zf, 1, rel_target(ctx, a->n));
    return true;
}

static bool trans_BRA(DisasContext *ctx, arg_BRA *a)
{
    gen_jump_direct(ctx, rel_target(ctx, a->n));
    return true;
}

static bool trans_RCALL(DisasContext *ctx, arg_RCALL *a)
{
    /* A stack overflow here can reset the board. */
    translator_io_start(&ctx->base);
    gen_helper_push_stack(tcg_env, tcg_constant_i32(ctx->npc));
    gen_jump_direct(ctx, rel_target(ctx, a->n));
    return true;
}

static bool trans_CALL(DisasContext *ctx, arg_CALL *a)
{
    uint32_t k = a->k | ((next_word(ctx) & 0xFFF) << 8);

    translator_io_start(&ctx->base);
    gen_helper_push_stack(tcg_env, tcg_constant_i32(ctx->npc));
    if (a->s) {
        gen_helper_call_shadow(tcg_env);
    }
    gen_jump_direct(ctx, k << 1);
    return true;
}

static bool trans_GOTO(DisasContext *ctx, arg_GOTO *a)
{
    uint32_t k = a->k | ((next_word(ctx) & 0xFFF) << 8);

    gen_jump_direct(ctx, k << 1);
    return true;
}

static bool trans_CALLW(DisasContext *ctx, arg_CALLW *a)
{
    TCGv dest = tcg_temp_new_i32();

    translator_io_start(&ctx->base);
    tcg_gen_shli_i32(dest, cpu_pclatu, 16);
    tcg_gen_deposit_i32(dest, dest, cpu_pclath, 8, 8);
    tcg_gen_deposit_i32(dest, dest, cpu_wreg, 0, 8);
    gen_helper_push_stack(tcg_env, tcg_constant_i32(ctx->npc));
    gen_jump_indirect(ctx, dest);
    return true;
}

static bool trans_RETFIE(DisasContext *ctx, arg_RETFIE *a)
{
    TCGv dest = tcg_temp_new_i32();

    /* A stack underflow here can reset the board. */
    translator_io_start(&ctx->base);
    gen_helper_retfie(dest, tcg_env, tcg_constant_i32(a->s));
    /* Leaving an ISR may uncover a pending lower-priority request. */
    gen_jump_exit(ctx, dest);
    return true;
}

static bool trans_RETLW(DisasContext *ctx, arg_RETLW *a)
{
    TCGv dest = tcg_temp_new_i32();

    translator_io_start(&ctx->base);
    tcg_gen_movi_i32(cpu_wreg, a->k);
    gen_helper_pop_stack(dest, tcg_env);
    gen_jump_indirect(ctx, dest);
    return true;
}

static bool trans_RETURN(DisasContext *ctx, arg_RETURN *a)
{
    TCGv dest = tcg_temp_new_i32();

    translator_io_start(&ctx->base);
    gen_helper_pop_stack(dest, tcg_env);
    if (a->s) {
        gen_helper_return_shadow(tcg_env);
    }
    gen_jump_indirect(ctx, dest);
    return true;
}

/*
 * Inherent operations
 */

static bool trans_CLRWDT(DisasContext *ctx, arg_CLRWDT *a)
{
    /*
     * Clearing the watchdog reaches a device timer, so the block has to be
     * allowed to do I/O -- under -icount a clock read from a helper that is
     * not marked this way aborts.
     */
    translator_io_start(&ctx->base);
    gen_helper_clrwdt(tcg_env);
    return true;
}

static bool trans_DAW(DisasContext *ctx, arg_DAW *a)
{
    gen_helper_daw(tcg_env);
    return true;
}

static bool trans_NOP(DisasContext *ctx, arg_NOP *a)
{
    return true;
}

static bool trans_POP(DisasContext *ctx, arg_POP *a)
{
    TCGv discard = tcg_temp_new_i32();

    translator_io_start(&ctx->base);
    gen_helper_pop_stack(discard, tcg_env);
    return true;
}

static bool trans_PUSH(DisasContext *ctx, arg_PUSH *a)
{
    translator_io_start(&ctx->base);
    gen_helper_push_stack(tcg_env, tcg_constant_i32(ctx->npc));
    return true;
}

static bool trans_RESET(DisasContext *ctx, arg_RESET *a)
{
    /*
     * Resetting the board is a main-loop operation, and reaching it from
     * translated code needs the same I/O context an MMIO write handler runs
     * in. The PC is committed first so the state is coherent while the
     * request is in flight.
     */
    tcg_gen_movi_i32(cpu_pc, ctx->npc);
    translator_io_start(&ctx->base);
    gen_helper_reset(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_SLEEP(DisasContext *ctx, arg_SLEEP *a)
{
    tcg_gen_movi_i32(cpu_pc, ctx->npc);
    gen_helper_sleep(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/*
 * Literal operations
 */

static bool trans_ADDFSR(DisasContext *ctx, arg_ADDFSR *a)
{
    tcg_gen_addi_i32(cpu_fsr[a->fn], cpu_fsr[a->fn], a->k);
    tcg_gen_andi_i32(cpu_fsr[a->fn], cpu_fsr[a->fn], PIC18_FSR_MASK);
    return true;
}

static bool trans_SUBFSR(DisasContext *ctx, arg_SUBFSR *a)
{
    tcg_gen_subi_i32(cpu_fsr[a->fn], cpu_fsr[a->fn], a->k);
    tcg_gen_andi_i32(cpu_fsr[a->fn], cpu_fsr[a->fn], PIC18_FSR_MASK);
    return true;
}

static bool trans_ADDLW(DisasContext *ctx, arg_ADDLW *a)
{
    gen_add8(cpu_wreg, cpu_wreg, tcg_constant_i32(a->k),
             tcg_constant_i32(0));
    return true;
}

static bool trans_ANDLW(DisasContext *ctx, arg_ANDLW *a)
{
    tcg_gen_andi_i32(cpu_wreg, cpu_wreg, a->k);
    gen_flag_zn(cpu_wreg);
    return true;
}

static bool trans_IORLW(DisasContext *ctx, arg_IORLW *a)
{
    tcg_gen_ori_i32(cpu_wreg, cpu_wreg, a->k);
    gen_flag_zn(cpu_wreg);
    return true;
}

static bool trans_LFSR(DisasContext *ctx, arg_LFSR *a)
{
    uint32_t k;

    if (a->fn > 2) {
        return false;
    }
    k = (a->k << 10) | (next_word(ctx) & 0x3FF);
    tcg_gen_movi_i32(cpu_fsr[a->fn], k);
    return true;
}

static bool trans_MOVLB(DisasContext *ctx, arg_MOVLB *a)
{
    tcg_gen_movi_i32(cpu_bsr, a->k);
    return true;
}

static bool trans_MOVLW(DisasContext *ctx, arg_MOVLW *a)
{
    tcg_gen_movi_i32(cpu_wreg, a->k);
    return true;
}

static bool trans_MULLW(DisasContext *ctx, arg_MULLW *a)
{
    TCGv val = tcg_temp_new_i32();

    tcg_gen_muli_i32(val, cpu_wreg, a->k);
    tcg_gen_andi_i32(cpu_prodl, val, 0xFF);
    tcg_gen_extract_i32(cpu_prodh, val, 8, 8);
    return true;
}

static bool trans_SUBLW(DisasContext *ctx, arg_SUBLW *a)
{
    /* k - W, computed as k + ~W + 1. */
    gen_add8(cpu_wreg, tcg_constant_i32(a->k), gen_not8(cpu_wreg),
             tcg_constant_i32(1));
    return true;
}

static bool trans_XORLW(DisasContext *ctx, arg_XORLW *a)
{
    tcg_gen_xori_i32(cpu_wreg, cpu_wreg, a->k);
    gen_flag_zn(cpu_wreg);
    return true;
}

/*
 * Data memory - program memory operations
 */

static bool trans_TBLRD(DisasContext *ctx, arg_TBLRD *a)
{
    gen_helper_tblrd(tcg_env, tcg_constant_i32(a->mm));
    return true;
}

static bool trans_TBLWT(DisasContext *ctx, arg_TBLWT *a)
{
    gen_helper_tblwt(tcg_env, tcg_constant_i32(a->mm));
    return true;
}

/*
 * Translator hooks
 */

static void translate(DisasContext *ctx)
{
    uint32_t opcode = translator_lduw(ctx->env, &ctx->base, ctx->npc);

    ctx->npc += 2;

    if (!decode_insn(ctx, opcode)) {
        gen_helper_unsupported(tcg_env);
        ctx->base.is_jmp = DISAS_NORETURN;
    }

    /* Whatever the indirect operands still owe, now the accesses are done. */
    gen_flush_fsr(ctx);
}

static void pic18_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    ctx->cs = cs;
    ctx->env = cpu_env(cs);
    ctx->npc = ctx->base.pc_first;
    ctx->need_exit = false;
    ctx->npend = 0;
}

static void pic18_tr_tb_start(DisasContextBase *db, CPUState *cs)
{
}

static void pic18_tr_insn_start(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    tcg_gen_insn_start(ctx->npc, 0, 0);
}

static void pic18_tr_translate_insn(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    translate(ctx);

    ctx->base.pc_next = ctx->npc;

    if (ctx->base.is_jmp == DISAS_NEXT) {
        target_ulong page_first = ctx->base.pc_first & TARGET_PAGE_MASK;

        /*
         * Stop before the page end so a three-word instruction fits, and
         * end the block after anything that touched the interrupt enables.
         */
        if (ctx->need_exit) {
            ctx->base.is_jmp = DISAS_TOO_MANY;
        } else if ((ctx->base.pc_next - page_first) >= TARGET_PAGE_SIZE - 8) {
            ctx->base.is_jmp = DISAS_TOO_MANY;
        }
    }
}

static void pic18_tr_tb_stop(DisasContextBase *dcbase, CPUState *cs)
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

static const TranslatorOps pic18_tr_ops = {
    .init_disas_context = pic18_tr_init_disas_context,
    .tb_start           = pic18_tr_tb_start,
    .insn_start         = pic18_tr_insn_start,
    .translate_insn     = pic18_tr_translate_insn,
    .tb_stop            = pic18_tr_tb_stop,
};

void pic18_cpu_translate_code(CPUState *cs, TranslationBlock *tb,
                              int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext dc = { };
    translator_loop(cs, tb, max_insns, pc, host_pc, &pic18_tr_ops, &dc.base,
                    TCG_TYPE_VA);
}

void pic18_cpu_tcg_init(void)
{
    cpu_pc = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(pc), "pc");

    cpu_wreg = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(wreg), "W");
    cpu_bsr = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(bsr), "BSR");
    cpu_pclath = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(pclath),
                                        "PCLATH");
    cpu_pclatu = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(pclatu),
                                        "PCLATU");
    cpu_intcon0 = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(intcon0),
                                         "INTCON0");
    cpu_fsr[0] = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(fsr[0]),
                                        "FSR0");
    cpu_fsr[1] = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(fsr[1]),
                                        "FSR1");
    cpu_fsr[2] = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(fsr[2]),
                                        "FSR2");
    cpu_prodl = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(prodl),
                                       "PRODL");
    cpu_prodh = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(prodh),
                                       "PRODH");
    cpu_tablat = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(tablat),
                                        "TABLAT");
    cpu_tblptr = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(tblptr),
                                        "TBLPTR");

    cpu_Cf = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(sregC), "Cf");
    cpu_DCf = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(sregDC), "DCf");
    cpu_Zf = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(sregZ), "Zf");
    cpu_OVf = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(sregOV), "OVf");
    cpu_Nf = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(sregN), "Nf");
    cpu_PDf = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(sregPD), "PDf");
    cpu_TOf = tcg_global_mem_new_i32(tcg_env, PIC18_REG_OFFS(sregTO), "TOf");
}
