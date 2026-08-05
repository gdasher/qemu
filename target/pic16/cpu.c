/*
 * PIC16 (enhanced mid-range) CPU
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
#include "system/runstate.h"

static void pic16_cpu_set_pc(CPUState *cs, vaddr value)
{
    /* Externally the PC is a byte address; internally it counts words. */
    cpu_env(cs)->pc_w = value / 2;
}

static vaddr pic16_cpu_get_pc(CPUState *cs)
{
    return cpu_env(cs)->pc_w * 2;
}

static bool pic16_cpu_has_work(CPUState *cs)
{
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD | CPU_INTERRUPT_RESET)
           && cpu_interrupts_enabled(cpu_env(cs));
}

static int pic16_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return ifetch ? MMU_CODE_IDX : MMU_DATA_IDX;
}

static TCGTBCPUState pic16_get_tb_cpu_state(CPUState *cs)
{
    CPUPIC16State *env = cpu_env(cs);

    return (TCGTBCPUState){ .pc = env->pc_w * 2, .flags = 0 };
}

static void pic16_cpu_synchronize_from_tb(CPUState *cs,
                                          const TranslationBlock *tb)
{
    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu_env(cs)->pc_w = tb->pc / 2;
}

static void pic16_restore_state_to_opc(CPUState *cs,
                                       const TranslationBlock *tb,
                                       const uint64_t *data)
{
    cpu_env(cs)->pc_w = data[0];
}

static void pic16_cpu_reset_hold(Object *obj, ResetType type)
{
    CPUState *cs = CPU(obj);
    PIC16CPUClass *mcc = PIC16_CPU_GET_CLASS(obj);
    CPUPIC16State *env = cpu_env(cs);

    if (mcc->parent_phases.hold) {
        mcc->parent_phases.hold(obj, type);
    }

    env->pc_w = PIC16_RESET_VECTOR;

    env->wreg = 0;
    env->bsr = 0;
    env->pclath = 0;
    env->intcon = 0;
    env->fsr[0] = 0;
    env->fsr[1] = 0;

    env->sregC = 0;
    env->sregDC = 0;
    env->sregZ = 0;
    /* TO and PD read 1 out of power-on reset. */
    env->sregPD = 1;
    env->sregTO = 1;

    memset(env->stack, 0, sizeof(env->stack));
    env->stkptr = PIC16_STKPTR_EMPTY;
    /*
     * stkovf, stkunf and reset_ri are reset causes and must survive the reset
     * they caused; software clears them through PCON0.
     */

    env->shadow_wreg = 0;
    env->shadow_status = 0;
    env->shadow_bsr = 0;
    env->shadow_fsr[0] = 0;
    env->shadow_fsr[1] = 0;
    env->shadow_pclath = 0;

    env->intsrc = 0;
}

static void pic16_cpu_disas_set_info(const CPUState *cpu,
                                     disassemble_info *info)
{
    info->endian = BFD_ENDIAN_LITTLE;
    info->print_insn = pic16_print_insn;
}

static void pic16_cpu_realizefn(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    PIC16CPUClass *mcc = PIC16_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_exec_realizefn(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }
    qemu_init_vcpu(cs);
    cpu_reset(cs);

    mcc->parent_realize(dev, errp);
}

static void pic16_cpu_set_int(void *opaque, int irq, int level)
{
    PIC16CPU *cpu = opaque;
    CPUPIC16State *env = &cpu->env;
    CPUState *cs = CPU(cpu);
    uint64_t mask = 1ull << irq;

    if (level) {
        env->intsrc |= mask;
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        env->intsrc &= ~mask;
        if (env->intsrc == 0) {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        }
    }
}

static void pic16_reset_bh(void *opaque)
{
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void pic16_cpu_initfn(Object *obj)
{
    PIC16CPU *cpu = PIC16_CPU(obj);

    cpu->reset_bh = qemu_bh_new(pic16_reset_bh, cpu);

    qdev_init_gpio_in(DEVICE(cpu), pic16_cpu_set_int,
                      sizeof(cpu->env.intsrc) * 8);
    qdev_init_gpio_out_named(DEVICE(cpu), &cpu->clrwdt, "clrwdt", 1);

    /* Unprogrammed configuration words read as all ones. */
    memset(cpu->env.config, 0xFF, sizeof(cpu->env.config));
    for (int i = 0; i < PIC16_NUM_CONFIG; i++) {
        cpu->env.config[i] = 0x3FFF;
    }
}

static ObjectClass *pic16_cpu_class_by_name(const char *cpu_model)
{
    ObjectClass *oc;
    char *typename;

    oc = object_class_by_name(cpu_model);
    if (oc != NULL) {
        return oc;
    }

    /* Also accept the short model name, as -cpu reports it. */
    typename = g_strdup_printf(PIC16_CPU_TYPE_NAME("%s"), cpu_model);
    oc = object_class_by_name(typename);
    g_free(typename);

    if (oc != NULL && object_class_is_abstract(oc)) {
        return NULL;
    }
    return oc;
}

static void pic16_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUPIC16State *env = cpu_env(cs);
    int i;

    qemu_fprintf(f, "PC:     %04x (word)\n", env->pc_w);
    qemu_fprintf(f, "W:        %02x\n", env->wreg);
    qemu_fprintf(f, "BSR:      %02x\n", env->bsr);
    qemu_fprintf(f, "PCLATH:   %02x\n", env->pclath);
    qemu_fprintf(f, "INTCON:   %02x\n", env->intcon);
    qemu_fprintf(f, "FSR0:   %04x  FSR1: %04x\n", env->fsr[0], env->fsr[1]);
    qemu_fprintf(f, "STATUS: [ %c %c %c %c %c ]\n",
                 env->sregTO ? 'T' : '-',
                 env->sregPD ? 'P' : '-',
                 env->sregZ  ? 'Z' : '-',
                 env->sregDC ? 'D' : '-',
                 env->sregC  ? 'C' : '-');
    qemu_fprintf(f, "STKPTR:   %02x%s%s\n", env->stkptr,
                 env->stkovf ? " OVF" : "", env->stkunf ? " UNF" : "");
    for (i = 0; i < PIC16_STACK_DEPTH; i++) {
        qemu_fprintf(f, "  TOS[%2d]: %04x%s", i, env->stack[i],
                     (i % 4) == 3 ? "\n" : "");
    }
    qemu_fprintf(f, "\n");
}

#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps pic16_sysemu_ops = {
    .has_work = pic16_cpu_has_work,
    .get_phys_addr_debug = pic16_cpu_get_phys_addr_debug,
};

static const TCGCPUOps pic16_tcg_ops = {
    .guest_default_memory_order = 0,
    .mttcg_supported = false,
    .initialize = pic16_cpu_tcg_init,
    .translate_code = pic16_cpu_translate_code,
    .get_tb_cpu_state = pic16_get_tb_cpu_state,
    .synchronize_from_tb = pic16_cpu_synchronize_from_tb,
    .restore_state_to_opc = pic16_restore_state_to_opc,
    .mmu_index = pic16_cpu_mmu_index,
    .cpu_exec_interrupt = pic16_cpu_exec_interrupt,
    .cpu_exec_halt = pic16_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
    .tlb_fill = pic16_cpu_tlb_fill,
    .do_interrupt = pic16_cpu_do_interrupt,
    .pointer_wrap = cpu_pointer_wrap_uint32,
};

static void pic16_cpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    PIC16CPUClass *mcc = PIC16_CPU_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, pic16_cpu_realizefn,
                                    &mcc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, pic16_cpu_reset_hold, NULL,
                                       &mcc->parent_phases);

    cc->class_by_name = pic16_cpu_class_by_name;
    cc->dump_state = pic16_cpu_dump_state;
    cc->set_pc = pic16_cpu_set_pc;
    cc->get_pc = pic16_cpu_get_pc;
    dc->vmsd = &vms_pic16_cpu;
    cc->sysemu_ops = &pic16_sysemu_ops;
    cc->disas_set_info = pic16_cpu_disas_set_info;
    cc->tcg_ops = &pic16_tcg_ops;
}

/*
 * The enhanced mid-range core, shared by the PIC16F1xxxx families. Device
 * specifics (program size, banks implemented, peripheral set) belong to the
 * SoC model rather than the CPU.
 */
static void pic16_pic16f1_initfn(Object *obj)
{
    CPUPIC16State *env = cpu_env(CPU(obj));

    set_pic16_feature(env, PIC16_FEATURE_EEPROM);
}

#define DEFINE_PIC16_CPU_TYPE(model, initfn) \
    { \
        .parent = TYPE_PIC16_CPU, \
        .instance_init = initfn, \
        .name = PIC16_CPU_TYPE_NAME(model), \
    }

static const TypeInfo pic16_cpu_type_info[] = {
    {
        .name = TYPE_PIC16_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(PIC16CPU),
        .instance_align = __alignof(PIC16CPU),
        .instance_init = pic16_cpu_initfn,
        .class_size = sizeof(PIC16CPUClass),
        .class_init = pic16_cpu_class_init,
        .abstract = true,
    },
    DEFINE_PIC16_CPU_TYPE("pic16f1", pic16_pic16f1_initfn),
};

DEFINE_TYPES(pic16_cpu_type_info)
