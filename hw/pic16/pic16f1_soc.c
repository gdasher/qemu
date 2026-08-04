/*
 * PIC16F1xxxx SoC
 *
 * Builds the memory map and the parts of the chip that belong to the core
 * rather than to a peripheral: the interrupt fan-in, the reset-cause
 * registers, and the bank 63 window onto the hardware stack and the interrupt
 * shadow registers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "migration/vmstate.h"
#include "pic16f1_soc.h"

/* Data addresses of the register blocks the SoC owns. */
#define PIC16_PIR0_ADDR     0x08C   /* PIR0-7, then a hole, then PIE0-7 */
#define PIC16_INTC_SIZE     0x014
#define PIC16_PIE_OFFSET    0x00A

#define PIC16_PCON0_ADDR    0x192
#define PIC16_PCON_SIZE     0x002

#define PIC16_CORE_SFR_ADDR 0x1FE4  /* shadow registers, STKPTR, TOSL, TOSH */
#define PIC16_CORE_SFR_SIZE 0x00C

/*
 * PCON0 reset-cause flags read 1 unless the matching reset happened. Stack
 * overflow and underflow are the two bits the core actually drives.
 */
#define PCON0_STKOVF 7
#define PCON0_STKUNF 6
#define PCON0_RESET  0x3C

static void pic16f1_soc_update_irq(PIC16F1SocState *s)
{
    bool pending = false;
    int i;

    for (i = 0; i < PIC16_NUM_PIR; i++) {
        if (s->pir[i] & s->pie[i]) {
            pending = true;
            break;
        }
    }

    /*
     * GIE and PEIE gate this in the CPU, where INTCON lives, so the SoC only
     * reports whether an enabled flag is set.
     */
    qemu_set_irq(s->cpu_irq, pending);
}

/*
 * A peripheral raising its line sets the PIR flag; only software clears it
 * again, which is how the hardware behaves for every latching flag. Flags
 * that hardware holds rather than latches (the EUSART's, for instance) are
 * kept asserted by their peripheral instead.
 */
static void pic16f1_soc_set_irq(void *opaque, int n, int level)
{
    PIC16F1SocState *s = opaque;

    if (level) {
        s->pir[n / 8] |= 1u << (n % 8);
        pic16f1_soc_update_irq(s);
    }
}

static uint64_t pic16f1_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16F1SocState *s = opaque;

    if (addr < PIC16_NUM_PIR) {
        return s->pir[addr];
    }
    if (addr >= PIC16_PIE_OFFSET && addr < PIC16_PIE_OFFSET + PIC16_NUM_PIR) {
        return s->pie[addr - PIC16_PIE_OFFSET];
    }
    return 0;
}

static void pic16f1_intc_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    PIC16F1SocState *s = opaque;

    if (addr < PIC16_NUM_PIR) {
        s->pir[addr] = value;
    } else if (addr >= PIC16_PIE_OFFSET &&
               addr < PIC16_PIE_OFFSET + PIC16_NUM_PIR) {
        s->pie[addr - PIC16_PIE_OFFSET] = value;
    } else {
        return;
    }
    pic16f1_soc_update_irq(s);
}

static const MemoryRegionOps pic16f1_intc_ops = {
    .read = pic16f1_intc_read,
    .write = pic16f1_intc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static uint64_t pic16f1_pcon_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16F1SocState *s = opaque;
    CPUPIC16State *env = &s->cpu.env;

    if (addr == 0) {
        uint8_t value = s->pcon0 & ~((1u << PCON0_STKOVF) |
                                     (1u << PCON0_STKUNF));
        value |= env->stkovf << PCON0_STKOVF;
        value |= env->stkunf << PCON0_STKUNF;
        return value;
    }
    return s->pcon1;
}

static void pic16f1_pcon_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    PIC16F1SocState *s = opaque;
    CPUPIC16State *env = &s->cpu.env;

    if (addr == 0) {
        s->pcon0 = value;
        env->stkovf = (value >> PCON0_STKOVF) & 1;
        env->stkunf = (value >> PCON0_STKUNF) & 1;
    } else {
        s->pcon1 = value;
    }
}

static const MemoryRegionOps pic16f1_pcon_ops = {
    .read = pic16f1_pcon_read,
    .write = pic16f1_pcon_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/*
 * Bank 63 exposes the interrupt shadow registers and a window onto the
 * hardware stack. Both are readable and writable: firmware can change what an
 * interrupt return will restore, and can walk the stack by moving STKPTR.
 */
enum {
    CORE_SFR_STATUS_SHAD,
    CORE_SFR_WREG_SHAD,
    CORE_SFR_BSR_SHAD,
    CORE_SFR_PCLATH_SHAD,
    CORE_SFR_FSR0L_SHAD,
    CORE_SFR_FSR0H_SHAD,
    CORE_SFR_FSR1L_SHAD,
    CORE_SFR_FSR1H_SHAD,
    CORE_SFR_RESERVED,
    CORE_SFR_STKPTR,
    CORE_SFR_TOSL,
    CORE_SFR_TOSH,
};

static uint64_t pic16f1_core_sfr_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16F1SocState *s = opaque;
    CPUPIC16State *env = &s->cpu.env;
    uint32_t tos;

    switch (addr) {
    case CORE_SFR_STATUS_SHAD:
        return env->shadow_status;
    case CORE_SFR_WREG_SHAD:
        return env->shadow_wreg;
    case CORE_SFR_BSR_SHAD:
        return env->shadow_bsr;
    case CORE_SFR_PCLATH_SHAD:
        return env->shadow_pclath;
    case CORE_SFR_FSR0L_SHAD:
        return env->shadow_fsr[0] & 0xFF;
    case CORE_SFR_FSR0H_SHAD:
        return (env->shadow_fsr[0] >> 8) & 0xFF;
    case CORE_SFR_FSR1L_SHAD:
        return env->shadow_fsr[1] & 0xFF;
    case CORE_SFR_FSR1H_SHAD:
        return (env->shadow_fsr[1] >> 8) & 0xFF;
    case CORE_SFR_STKPTR:
        return env->stkptr;
    case CORE_SFR_TOSL:
    case CORE_SFR_TOSH:
        /* Reading the top of an empty stack returns zero. */
        tos = env->stkptr < PIC16_STACK_DEPTH ? env->stack[env->stkptr] : 0;
        return addr == CORE_SFR_TOSL ? tos & 0xFF : (tos >> 8) & 0x7F;
    default:
        return 0;
    }
}

static void pic16f1_core_sfr_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    PIC16F1SocState *s = opaque;
    CPUPIC16State *env = &s->cpu.env;

    switch (addr) {
    case CORE_SFR_STATUS_SHAD:
        env->shadow_status = value;
        break;
    case CORE_SFR_WREG_SHAD:
        env->shadow_wreg = value;
        break;
    case CORE_SFR_BSR_SHAD:
        env->shadow_bsr = value & 0x3F;
        break;
    case CORE_SFR_PCLATH_SHAD:
        env->shadow_pclath = value & 0x7F;
        break;
    case CORE_SFR_FSR0L_SHAD:
        env->shadow_fsr[0] = (env->shadow_fsr[0] & 0xFF00) | (value & 0xFF);
        break;
    case CORE_SFR_FSR0H_SHAD:
        env->shadow_fsr[0] = (env->shadow_fsr[0] & 0xFF) | (value << 8);
        break;
    case CORE_SFR_FSR1L_SHAD:
        env->shadow_fsr[1] = (env->shadow_fsr[1] & 0xFF00) | (value & 0xFF);
        break;
    case CORE_SFR_FSR1H_SHAD:
        env->shadow_fsr[1] = (env->shadow_fsr[1] & 0xFF) | (value << 8);
        break;
    case CORE_SFR_STKPTR:
        env->stkptr = value & PIC16_STKPTR_MASK;
        break;
    case CORE_SFR_TOSL:
        if (env->stkptr < PIC16_STACK_DEPTH) {
            env->stack[env->stkptr] =
                (env->stack[env->stkptr] & 0x7F00) | (value & 0xFF);
        }
        break;
    case CORE_SFR_TOSH:
        if (env->stkptr < PIC16_STACK_DEPTH) {
            env->stack[env->stkptr] =
                (env->stack[env->stkptr] & 0xFF) | ((value & 0x7F) << 8);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pic16f1_core_sfr_ops = {
    .read = pic16f1_core_sfr_read,
    .write = pic16f1_core_sfr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic16f1_soc_realize(DeviceState *dev, Error **errp)
{
    PIC16F1SocState *s = PIC16F1_SOC(dev);
    PIC16F1SocClass *sc = PIC16F1_SOC_GET_CLASS(dev);
    MemoryRegion *system_memory = get_system_memory();
    unsigned i;

    object_initialize_child(OBJECT(dev), "cpu", &s->cpu, sc->cpu_type);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_abort);

    s->fosc = clock_new(OBJECT(dev), "fosc");
    clock_set_hz(s->fosc, sc->fosc_hz);

    /* Program flash, and the configuration words above word 0x8000. */
    memory_region_init_rom(&s->flash, OBJECT(dev), "pic16.flash",
                           sc->flash_words * 2, &error_fatal);
    memory_region_add_subregion(system_memory, OFFSET_CODE, &s->flash);

    memory_region_init_rom(&s->config, OBJECT(dev), "pic16.config",
                           PIC16_CONFIG_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, OFFSET_CONFIG, &s->config);

    /*
     * Anything in the banked data space that no device claims is logged
     * rather than silently reading zero, which is the quickest way to find
     * out what a firmware image actually touches.
     */
    create_unimplemented_device("pic16.sfr", OFFSET_DATA, PIC16_BANKED_SIZE);

    /*
     * Only the implemented banks get a GPR block. The linear window and the
     * common-RAM aliases need no regions of their own: indirect access is
     * resolved in target/pic16/helper.c, and direct access to 0x70-0x7F needs
     * no BSR, so common RAM exists once at its bank 0 address.
     */
    for (i = 0; i < sc->gpr_banks; i++) {
        g_autofree char *name = g_strdup_printf("pic16.gpr%u", i);

        memory_region_init_ram(&s->gpr[i], OBJECT(dev), name,
                               PIC16_GPR_SIZE, &error_fatal);
        memory_region_add_subregion(system_memory,
                                    OFFSET_DATA + i * PIC16_BANK_SIZE +
                                    PIC16_GPR_BASE, &s->gpr[i]);
    }

    memory_region_init_ram(&s->common, OBJECT(dev), "pic16.common",
                           PIC16_COMMON_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory,
                                OFFSET_DATA + PIC16_COMMON_BASE, &s->common);

    memory_region_init_io(&s->intc, OBJECT(dev), &pic16f1_intc_ops, s,
                          "pic16.intc", PIC16_INTC_SIZE);
    memory_region_add_subregion(system_memory,
                                OFFSET_DATA + PIC16_PIR0_ADDR, &s->intc);

    memory_region_init_io(&s->pcon, OBJECT(dev), &pic16f1_pcon_ops, s,
                          "pic16.pcon", PIC16_PCON_SIZE);
    memory_region_add_subregion(system_memory,
                                OFFSET_DATA + PIC16_PCON0_ADDR, &s->pcon);

    memory_region_init_io(&s->core_sfr, OBJECT(dev), &pic16f1_core_sfr_ops, s,
                          "pic16.core-sfr", PIC16_CORE_SFR_SIZE);
    memory_region_add_subregion(system_memory,
                                OFFSET_DATA + PIC16_CORE_SFR_ADDR,
                                &s->core_sfr);

    s->cpu_irq = qdev_get_gpio_in(DEVICE(&s->cpu), 0);
    qdev_init_gpio_in_named(dev, pic16f1_soc_set_irq, PIC16_IRQ_GPIO,
                            PIC16_NUM_IRQ);
}

static void pic16f1_soc_reset_hold(Object *obj, ResetType type)
{
    PIC16F1SocState *s = PIC16F1_SOC(obj);

    memset(s->pir, 0, sizeof(s->pir));
    memset(s->pie, 0, sizeof(s->pie));
    s->pcon0 = PCON0_RESET;
    s->pcon1 = 0;
    pic16f1_soc_update_irq(s);
}

static const VMStateDescription pic16f1_soc_vmstate = {
    .name = "pic16f1-soc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(pir, PIC16F1SocState, PIC16_NUM_PIR),
        VMSTATE_UINT8_ARRAY(pie, PIC16F1SocState, PIC16_NUM_PIR),
        VMSTATE_UINT8(pcon0, PIC16F1SocState),
        VMSTATE_UINT8(pcon1, PIC16F1SocState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic16f1_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic16f1_soc_realize;
    dc->vmsd = &pic16f1_soc_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = pic16f1_soc_reset_hold;
}

static void pic16f17546_class_init(ObjectClass *oc, const void *data)
{
    PIC16F1SocClass *sc = PIC16F1_SOC_CLASS(oc);

    sc->cpu_type = PIC16_CPU_TYPE_NAME("pic16f1");
    sc->flash_words = 16 * 1024;
    /*
     * Banks 0-25 carry an 80-byte GPR block; 26 and above are unimplemented
     * and read as zero (DS40002637A figures 9-3 to 9-11).
     */
    sc->gpr_banks = 26;
    /* RSTOSC selects the 32 MHz HFINTOSC, and the firmware never changes it. */
    sc->fosc_hz = 32 * 1000 * 1000;
}

static const TypeInfo pic16f1_soc_types[] = {
    {
        .name = TYPE_PIC16F1_SOC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC16F1SocState),
        .class_size = sizeof(PIC16F1SocClass),
        .class_init = pic16f1_soc_class_init,
        .abstract = true,
    },
    {
        .name = TYPE_PIC16F17546_SOC,
        .parent = TYPE_PIC16F1_SOC,
        .class_init = pic16f17546_class_init,
    },
};

DEFINE_TYPES(pic16f1_soc_types)
