/*
 * PIC18 (Q43 core, standard instruction set) CPU
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/qemu-print.h"
#include "exec/translation-block.h"
#include "system/address-spaces.h"
#include "cpu.h"
#include "disas/dis-asm.h"
#include "tcg/debug-assert.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "accel/tcg/cpu-ops.h"
#include "system/reset.h"

static void pic18_cpu_set_pc(CPUState *cs, vaddr value)
{
    cpu_env(cs)->pc = value & PIC18_PC_MASK;
}

static vaddr pic18_cpu_get_pc(CPUState *cs)
{
    return cpu_env(cs)->pc;
}

/*
 * Whether a halted (sleeping) core should resume. An enabled request wakes
 * the core whether or not GIE is set (DS40002147F 11.11); if it is, the
 * interrupt is then taken, otherwise execution continues after SLEEP.
 */
static bool pic18_cpu_has_work(CPUState *cs)
{
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD | CPU_INTERRUPT_RESET)
           && cpu_env(cs)->intreq != 0;
}

static int pic18_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return ifetch ? MMU_CODE_IDX : MMU_DATA_IDX;
}

static TCGTBCPUState pic18_get_tb_cpu_state(CPUState *cs)
{
    CPUPIC18State *env = cpu_env(cs);

    return (TCGTBCPUState){ .pc = env->pc, .flags = 0 };
}

static void pic18_cpu_synchronize_from_tb(CPUState *cs,
                                          const TranslationBlock *tb)
{
    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu_env(cs)->pc = tb->pc;
}

static void pic18_restore_state_to_opc(CPUState *cs,
                                       const TranslationBlock *tb,
                                       const uint64_t *data)
{
    cpu_env(cs)->pc = data[0];
}

static void pic18_cpu_reset_hold(Object *obj, ResetType type)
{
    CPUState *cs = CPU(obj);
    PIC18CPUClass *mcc = PIC18_CPU_GET_CLASS(obj);
    CPUPIC18State *env = cpu_env(cs);

    if (mcc->parent_phases.hold) {
        mcc->parent_phases.hold(obj, type);
    }

    env->pc = PIC18_RESET_VECTOR;

    env->wreg = 0;
    env->bsr = 0;
    env->pclath = 0;
    env->pclatu = 0;
    env->intcon0 = PIC18_INTCON0_RESET;
    env->intstat = PIC18_INTSTAT_MAIN;
    env->cpudoze = 0;
    env->fsr[0] = 0;
    env->fsr[1] = 0;
    env->fsr[2] = 0;
    env->prodl = 0;
    env->prodh = 0;
    env->tablat = 0;
    env->tblptr = 0;

    env->sregC = 0;
    env->sregDC = 0;
    env->sregZ = 0;
    env->sregOV = 0;
    env->sregN = 0;
    /* TO and PD read 1 out of power-on reset. */
    env->sregPD = 1;
    env->sregTO = 1;

    memset(env->stack, 0, sizeof(env->stack));
    env->stkptr = 0;
    /*
     * stkovf, stkunf, reset_ri, reset_wdt and pcon0 are reset causes and must
     * survive the reset they caused; software clears them through PCON0.
     */

    env->shadow_status = 0;
    env->shadow_wreg = 0;
    env->shadow_bsr = 0;
    memset(env->shadow_fsr, 0, sizeof(env->shadow_fsr));
    env->shadow_prodl = 0;
    env->shadow_prodh = 0;
    env->shadow_pclath = 0;
    env->shadow_pclatu = 0;
    env->shadow_lo_status = 0;
    env->shadow_lo_wreg = 0;
    env->shadow_lo_bsr = 0;
    memset(env->shadow_lo_fsr, 0, sizeof(env->shadow_lo_fsr));
    env->shadow_lo_prodl = 0;
    env->shadow_lo_prodh = 0;
    env->shadow_lo_pclath = 0;
    env->shadow_lo_pclatu = 0;
    env->cshad_status = 0;
    env->cshad_wreg = 0;
    env->cshad_bsr = 0;

    /*
     * intreq is the state of the SoC's request lines, not CPU state, so it is
     * kept. The common reset dropped interrupt_request, though, so re-raise
     * it if a line is still up.
     */
    if (env->intreq) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

static void pic18_cpu_disas_set_info(const CPUState *cpu,
                                     disassemble_info *info)
{
    info->endian = BFD_ENDIAN_LITTLE;
    info->print_insn = pic18_print_insn;
}

/*
 * Resetting the machine walks the qbus tree, and a CPU hangs off no bus, so
 * nothing would reach it. Every target has to arrange this for itself; doing
 * it here rather than in each board means a new PIC18 machine cannot forget.
 * Without it a board reset leaves the core running where it was, with the
 * peripherals underneath it back at their power-on values.
 */
static void pic18_cpu_reset_handler(void *opaque)
{
    cpu_reset(CPU(opaque));
}

static void pic18_cpu_realizefn(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    PIC18CPUClass *mcc = PIC18_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_common_realize(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }
    qemu_init_vcpu(cs);
    cpu_reset(cs);
    qemu_register_reset(pic18_cpu_reset_handler, cs);

    mcc->parent_realize(dev, errp);
}

/* GPIO inputs: line PIC18_IRQ_HIGH and PIC18_IRQ_LOW, level driven. */
static void pic18_cpu_set_int(void *opaque, int irq, int level)
{
    PIC18CPU *cpu = opaque;
    CPUPIC18State *env = &cpu->env;
    CPUState *cs = CPU(cpu);
    uint32_t mask = 1u << irq;

    if (level) {
        env->intreq |= mask;
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        env->intreq &= ~mask;
        if (env->intreq == 0) {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        }
    }
}

static void pic18_cpu_initfn(Object *obj)
{
    PIC18CPU *cpu = PIC18_CPU(obj);

    qdev_init_gpio_in(DEVICE(cpu), pic18_cpu_set_int, 2);
    qdev_init_gpio_out_named(DEVICE(cpu), &cpu->clrwdt, "clrwdt", 1);

    /* Unprogrammed configuration bytes read as all ones. */
    memset(cpu->env.config, 0xFF, sizeof(cpu->env.config));
    for (int i = 0; i < PIC18_NUM_CONFIG; i++) {
        cpu->env.config[i] = 0xFF;
    }

    /* Power-on: POR and BOR flagged, no other cause. */
    cpu->env.pcon0 = PIC18_PCON0_POWERON;
}

static ObjectClass *pic18_cpu_class_by_name(const char *cpu_model)
{
    ObjectClass *oc;
    char *typename;

    oc = object_class_by_name(cpu_model);
    if (oc != NULL) {
        return oc;
    }

    /* Also accept the short model name, as -cpu reports it. */
    typename = g_strdup_printf(PIC18_CPU_TYPE_NAME("%s"), cpu_model);
    oc = object_class_by_name(typename);
    g_free(typename);

    if (oc != NULL && object_class_is_abstract(oc)) {
        return NULL;
    }
    return oc;
}

static void pic18_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUPIC18State *env = cpu_env(cs);
    int i, n;

    qemu_fprintf(f, "PC:     %06x\n", env->pc);
    qemu_fprintf(f, "W:        %02x  BSR: %02x  PCLATU:PCLATH: %02x:%02x\n",
                 env->wreg, env->bsr, env->pclatu, env->pclath);
    qemu_fprintf(f, "STATUS: [ %c %c %c %c %c %c %c ]  (0x%02x)\n",
                 env->sregTO ? 'T' : '-',
                 env->sregPD ? 'P' : '-',
                 env->sregN  ? 'N' : '-',
                 env->sregOV ? 'O' : '-',
                 env->sregZ  ? 'Z' : '-',
                 env->sregDC ? 'D' : '-',
                 env->sregC  ? 'C' : '-',
                 cpu_get_status(env));
    qemu_fprintf(f, "INTCON0:  %02x  STAT: %d  REQ: %x\n",
                 env->intcon0, env->intstat, env->intreq);
    qemu_fprintf(f, "FSR0:   %04x  FSR1: %04x  FSR2: %04x\n",
                 env->fsr[0], env->fsr[1], env->fsr[2]);
    qemu_fprintf(f, "PRODH:PRODL: %02x:%02x  TABLAT: %02x  TBLPTR: %06x\n",
                 env->prodh, env->prodl, env->tablat, env->tblptr);
    qemu_fprintf(f, "STKPTR:   %02x%s%s\n", env->stkptr,
                 env->stkovf ? " OVF" : "", env->stkunf ? " UNF" : "");
    n = MIN(env->stkptr, PIC18_STACK_DEPTH);
    for (i = 1; i <= n; i++) {
        qemu_fprintf(f, "  TOS[%3d]: %06x%s", i, env->stack[i],
                     (i % 4) == 0 ? "\n" : "");
    }
    if (n % 4) {
        qemu_fprintf(f, "\n");
    }
}

#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps pic18_sysemu_ops = {
    .has_work = pic18_cpu_has_work,
    .get_phys_addr_debug = pic18_cpu_get_phys_addr_debug,
};

static const TCGCPUOps pic18_tcg_ops = {
    .guest_default_memory_order = 0,
    .mttcg_supported = false,
    .initialize = pic18_cpu_tcg_init,
    .translate_code = pic18_cpu_translate_code,
    .get_tb_cpu_state = pic18_get_tb_cpu_state,
    .synchronize_from_tb = pic18_cpu_synchronize_from_tb,
    .restore_state_to_opc = pic18_restore_state_to_opc,
    .mmu_index = pic18_cpu_mmu_index,
    .cpu_exec_interrupt = pic18_cpu_exec_interrupt,
    .cpu_exec_halt = pic18_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
    .tlb_fill = pic18_cpu_tlb_fill,
    .do_interrupt = pic18_cpu_do_interrupt,
    .pointer_wrap = cpu_pointer_wrap_uint32,
};

static void pic18_cpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    PIC18CPUClass *mcc = PIC18_CPU_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, pic18_cpu_realizefn,
                                    &mcc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, pic18_cpu_reset_hold, NULL,
                                       &mcc->parent_phases);

    cc->class_by_name = pic18_cpu_class_by_name;
    cc->dump_state = pic18_cpu_dump_state;
    cc->set_pc = pic18_cpu_set_pc;
    cc->get_pc = pic18_cpu_get_pc;
    dc->vmsd = &vms_pic18_cpu;
    cc->sysemu_ops = &pic18_sysemu_ops;
    cc->disas_set_info = pic18_cpu_disas_set_info;
    cc->tcg_ops = &pic18_tcg_ops;
}

/*
 * The PIC18F-Q43 core. Device specifics (flash size, banks implemented,
 * peripheral set) belong to the SoC model rather than the CPU.
 */
static void pic18_pic18q43_initfn(Object *obj)
{
}

#define DEFINE_PIC18_CPU_TYPE(model, initfn) \
    { \
        .parent = TYPE_PIC18_CPU, \
        .instance_init = initfn, \
        .name = PIC18_CPU_TYPE_NAME(model), \
    }

static const TypeInfo pic18_cpu_type_info[] = {
    {
        .name = TYPE_PIC18_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(PIC18CPU),
        .instance_align = __alignof(PIC18CPU),
        .instance_init = pic18_cpu_initfn,
        .class_size = sizeof(PIC18CPUClass),
        .class_init = pic18_cpu_class_init,
        .abstract = true,
    },
    DEFINE_PIC18_CPU_TYPE("pic18q43", pic18_pic18q43_initfn),
};

DEFINE_TYPES(pic18_cpu_type_info)
