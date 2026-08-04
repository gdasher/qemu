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

static TCGv cpu_skip;

#define PIC16_REG_OFFS(x) offsetof(CPUPIC16State, x)

typedef struct DisasContext {
    DisasContextBase base;

    CPUPIC16State *env;
    CPUState *cs;

    /*
     * Instruction addresses count words, while DisasContextBase works in
     * bytes; npc is the word address of the next instruction.
     */
    uint32_t npc;
} DisasContext;

/* Include the auto-generated decoder. */
static bool decode_insn(DisasContext *ctx, uint16_t insn);
#include "decode-insn.c.inc"

static bool use_goto_tb(DisasContext *ctx, uint32_t dest)
{
    return translator_use_goto_tb(&ctx->base, dest * 2);
}

static void gen_goto_tb(DisasContext *ctx, int n, uint32_t dest)
{
    if (use_goto_tb(ctx, dest)) {
        tcg_gen_goto_tb(n);
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_exit_tb(ctx->base.tb, n);
    } else {
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_lookup_and_goto_ptr();
    }
    ctx->base.is_jmp = DISAS_NORETURN;
}

/*
 * Instruction translation
 */

static bool trans_NOP(DisasContext *ctx, arg_NOP *a)
{
    return true;
}

static bool trans_MOVLW(DisasContext *ctx, arg_MOVLW *a)
{
    tcg_gen_movi_i32(cpu_wreg, a->k);
    return true;
}

static bool trans_GOTO(DisasContext *ctx, arg_GOTO *a)
{
    /*
     * GOTO supplies PC[10:0]; PCLATH[6:3] supplies PC[14:11]. PCLATH is not
     * known at translation time, so the target is computed at runtime unless
     * a later phase proves it constant.
     */
    TCGv dest = tcg_temp_new_i32();

    tcg_gen_andi_i32(dest, cpu_pclath, 0x78);
    tcg_gen_shli_i32(dest, dest, 8);
    tcg_gen_ori_i32(dest, dest, a->k);
    tcg_gen_mov_i32(cpu_pc, dest);

    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static void translate(DisasContext *ctx)
{
    uint32_t opcode = translator_lduw(ctx->env, &ctx->base, ctx->npc * 2);

    ctx->npc++;

    if (!decode_insn(ctx, opcode)) {
        gen_helper_unsupported(tcg_env);
        ctx->base.is_jmp = DISAS_NORETURN;
    }
}

/*
 * Translator hooks
 */

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

    cpu_skip = tcg_global_mem_new_i32(tcg_env, PIC16_REG_OFFS(skip), "skip");
}
