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
#include "hw/core/qdev-properties-system.h"
#include "system/system.h"
#include "pic16f1_soc.h"
#include <math.h>

#define PIC16_INTC_SIZE     0x014
#define PIC16_PIE_OFFSET    0x00A

#define PIC16_PCON_SIZE     0x002

#define PIC16_CORE_SFR_SIZE 0x00C

/*
 * PCON0 records why the device last reset. Except for the two stack flags,
 * which are set by the event, these read 1 normally and 0 for the cause that
 * fired -- so the register is assembled from the CPU and watchdog state rather
 * than stored.
 */
#define PCON0_STKOVF 7
#define PCON0_STKUNF 6
#define PCON0_WDTWV  5
#define PCON0_RWDT   4
#define PCON0_RMCLR  3
#define PCON0_RI     2
#define PCON0_POR    1
#define PCON0_BOR    0

static void pic16f1_soc_update_irq(PIC16F1SocState *s)
{
    bool pending = false;
    int i;

    for (i = 0; i < PIC16_NUM_PIR; i++) {
        if ((s->pir_latch[i] | s->pir_level[i]) & s->pie[i]) {
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
        s->pir_latch[n / 8] |= 1u << (n % 8);
        pic16f1_soc_update_irq(s);
    }
}

/*
 * A level line is held by its peripheral for as long as the condition lasts.
 * Software cannot clear the flag; it dismisses the interrupt by acting on the
 * peripheral, which then drops the line.
 */
static void pic16f1_soc_set_irq_level(void *opaque, int n, int level)
{
    PIC16F1SocState *s = opaque;
    uint8_t mask = 1u << (n % 8);

    if (level) {
        s->pir_level[n / 8] |= mask;
    } else {
        s->pir_level[n / 8] &= ~mask;
    }
    pic16f1_soc_update_irq(s);
}

static uint64_t pic16f1_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16F1SocState *s = opaque;

    if (addr < PIC16_NUM_PIR) {
        return s->pir_latch[addr] | s->pir_level[addr];
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
        /* Only the latched flags are software-writable. */
        s->pir_latch[addr] = value & ~s->pir_level[addr];
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
    uint8_t value;

    switch (addr) {
    case 0:
        value = (1u << PCON0_WDTWV) | (1u << PCON0_RMCLR);

        if (env->stkovf) {
            value |= 1u << PCON0_STKOVF;
        }
        if (env->stkunf) {
            value |= 1u << PCON0_STKUNF;
        }
        if (!s->wwdt.expired) {
            value |= 1u << PCON0_RWDT;
        }
        if (!env->reset_ri) {
            value |= 1u << PCON0_RI;
        }
        if (!s->por) {
            value |= 1u << PCON0_POR;
        }
        if (!s->bor) {
            value |= 1u << PCON0_BOR;
        }
        return value;
    case 1:
        return s->pcon1;
    default:
        return 0;
    }
}

static void pic16f1_pcon_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    PIC16F1SocState *s = opaque;
    CPUPIC16State *env = &s->cpu.env;

    switch (addr) {
    case 0:
        if (!(value & (1u << PCON0_STKOVF))) {
            env->stkovf = 0;
        }
        if (!(value & (1u << PCON0_STKUNF))) {
            env->stkunf = 0;
        }
        if (!(value & (1u << PCON0_RWDT))) {
            s->wwdt.expired = false;
        }
        if (value & (1u << PCON0_RI)) {
            env->reset_ri = 0;
        }
        if (value & (1u << PCON0_POR)) {
            s->por = false;
        }
        if (value & (1u << PCON0_BOR)) {
            s->bor = false;
        }
        break;
    case 1:
        s->pcon1 = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pic16f1_pcon_ops = {
    .read = pic16f1_pcon_read,
    .write = pic16f1_pcon_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static uint64_t pic16f1_core_sfr_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16F1SocState *s = opaque;
    CPUPIC16State *env = &s->cpu.env;

    switch (addr) {
    case 0x00: /* STATUS_SHAD */
        return env->shadow_status;
    case 0x01: /* WREG_SHAD */
        return env->shadow_wreg;
    case 0x02: /* BSR_SHAD */
        return env->shadow_bsr;
    case 0x03: /* PCLATH_SHAD */
        return env->shadow_pclath;
    case 0x04: /* FSR0L_SHAD */
        return env->shadow_fsr[0] & 0xFF;
    case 0x05: /* FSR0H_SHAD */
        return (env->shadow_fsr[0] >> 8) & 0xFF;
    case 0x06: /* FSR1L_SHAD */
        return env->shadow_fsr[1] & 0xFF;
    case 0x07: /* FSR1H_SHAD */
        return (env->shadow_fsr[1] >> 8) & 0xFF;
    case 0x09: /* STKPTR */
        return env->stkptr;
    case 0x0A: /* TOSL */
        return env->stack[env->stkptr] & 0xFF;
    case 0x0B: /* TOSH */
        return (env->stack[env->stkptr] >> 8) & 0x7F;
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
    case 0x00: /* STATUS_SHAD */
        env->shadow_status = value & 0x07;
        break;
    case 0x01: /* WREG_SHAD */
        env->shadow_wreg = value & 0xFF;
        break;
    case 0x02: /* BSR_SHAD */
        env->shadow_bsr = value & 0x3F;
        break;
    case 0x03: /* PCLATH_SHAD */
        env->shadow_pclath = value & 0x7F;
        break;
    case 0x04: /* FSR0L_SHAD */
        env->shadow_fsr[0] = (env->shadow_fsr[0] & 0xFF00) | (value & 0xFF);
        break;
    case 0x05: /* FSR0H_SHAD */
        env->shadow_fsr[0] = (env->shadow_fsr[0] & 0x00FF) |
                             ((value & 0xFF) << 8);
        break;
    case 0x06: /* FSR1L_SHAD */
        env->shadow_fsr[1] = (env->shadow_fsr[1] & 0xFF00) | (value & 0xFF);
        break;
    case 0x07: /* FSR1H_SHAD */
        env->shadow_fsr[1] = (env->shadow_fsr[1] & 0x00FF) |
                             ((value & 0xFF) << 8);
        break;
    case 0x09: /* STKPTR */
        env->stkptr = value & PIC16_STKPTR_MASK;
        break;
    case 0x0A: /* TOSL */
        env->stack[env->stkptr] = (env->stack[env->stkptr] & 0x7F00) |
                                  (value & 0xFF);
        break;
    case 0x0B: /* TOSH */
        env->stack[env->stkptr] = (env->stack[env->stkptr] & 0x00FF) |
                                  ((value & 0x7F) << 8);
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

static uint64_t pic16f1_pps_out_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16F1SocState *s = opaque;

    if (addr < sizeof(s->pps_out_regs)) {
        return s->pps_out_regs[addr];
    }
    return 0;
}

static void pic16f1_pps_out_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    PIC16F1SocState *s = opaque;

    if (addr < sizeof(s->pps_out_regs)) {
        s->pps_out_regs[addr] = value;
    }
}

static const MemoryRegionOps pic16f1_pps_out_ops = {
    .read = pic16f1_pps_out_read,
    .write = pic16f1_pps_out_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static uint64_t pic16f1_pps_in_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16F1SocState *s = opaque;

    if (addr < sizeof(s->pps_in_regs)) {
        return s->pps_in_regs[addr];
    }
    return 0;
}

static void pic16f1_pps_in_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    PIC16F1SocState *s = opaque;

    if (addr < sizeof(s->pps_in_regs)) {
        s->pps_in_regs[addr] = value;
    }
}

static const MemoryRegionOps pic16f1_pps_in_ops = {
    .read = pic16f1_pps_in_read,
    .write = pic16f1_pps_in_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic16_drift_timer_cb(void *opaque)
{
    PIC16F1SocState *s = opaque;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    double t_sec = (double)now_ns * 1e-9;

    double thermal_ppm = 0.0;
    if (s->drift.thermal_tau_ms > 0) {
        double tau_sec = (double)s->drift.thermal_tau_ms * 1e-3;
        thermal_ppm = (double)s->drift.thermal_max_ppm * (1.0 - exp(-t_sec / tau_sec));
    }

    double linear_ppm = (double)s->drift.linear_ppm_per_s * t_sec;
    double ripple_ppm = 0.0;
    if (s->drift.ripple_amp_ppm != 0 && s->drift.ripple_freq_hz > 0.0) {
        ripple_ppm = (double)s->drift.ripple_amp_ppm * sin(2.0 * M_PI * s->drift.ripple_freq_hz * t_sec);
    }

    if (s->drift.jitter_sigma_ppm > 0) {
        double step = ((double)rand() / (double)RAND_MAX) - 0.5;
        s->drift.accumulated_jitter += step * (double)s->drift.jitter_sigma_ppm * 0.2;
        if (s->drift.accumulated_jitter > 3.0 * s->drift.jitter_sigma_ppm) {
            s->drift.accumulated_jitter = 3.0 * s->drift.jitter_sigma_ppm;
        } else if (s->drift.accumulated_jitter < -3.0 * s->drift.jitter_sigma_ppm) {
            s->drift.accumulated_jitter = -3.0 * s->drift.jitter_sigma_ppm;
        }
    }

    int8_t tune = (int8_t)((s->osc_regs[5] & 0x3F) | ((s->osc_regs[5] & 0x20) ? 0xC0 : 0));
    int32_t dynamic_drift_ppm = (int32_t)(thermal_ppm + linear_ppm + ripple_ppm + s->drift.accumulated_jitter);
    int32_t total_ppm = s->osc_ppm + (int32_t)tune * 1000 + dynamic_drift_ppm;

    int64_t tuned_hz = (int64_t)s->base_fosc_hz +
                       (int64_t)s->base_fosc_hz * total_ppm / 1000000;
    if (tuned_hz > 0 && s->fosc) {
        clock_set_hz(s->fosc, (uint64_t)tuned_hz);
        pic16_tmr0_update_fosc(&s->tmr0);
    }

    if (s->drift.enabled && s->drift_timer) {
        timer_mod(s->drift_timer, now_ns + 5 * 1000 * 1000); /* 5 ms virtual interval */
    }
}

void pic16f1_soc_parse_drift(PIC16F1SocState *s, const char *str)
{
    if (!str || !*str) {
        return;
    }

    s->drift.enabled = true;

    if (!strcmp(str, "thermal") || !strcmp(str, "warmup")) {
        s->drift.thermal_max_ppm = 3500;
        s->drift.thermal_tau_ms = 2000;
        return;
    } else if (!strcmp(str, "linear")) {
        s->drift.linear_ppm_per_s = 100;
        return;
    } else if (!strcmp(str, "ripple")) {
        s->drift.ripple_amp_ppm = 150;
        s->drift.ripple_freq_hz = 2.0;
        return;
    } else if (!strcmp(str, "jitter")) {
        s->drift.jitter_sigma_ppm = 20;
        return;
    } else if (!strcmp(str, "realistic")) {
        s->drift.thermal_max_ppm = 3000;
        s->drift.thermal_tau_ms = 2000;
        s->drift.linear_ppm_per_s = 20;
        s->drift.ripple_amp_ppm = 80;
        s->drift.ripple_freq_hz = 2.0;
        s->drift.jitter_sigma_ppm = 10;
        return;
    } else if (!strcmp(str, "stress")) {
        s->drift.thermal_max_ppm = 5000;
        s->drift.thermal_tau_ms = 1500;
        s->drift.linear_ppm_per_s = 50;
        s->drift.ripple_amp_ppm = 200;
        s->drift.ripple_freq_hz = 3.0;
        s->drift.jitter_sigma_ppm = 25;
        return;
    }

    g_auto(GStrv) tokens = g_strsplit_set(str, ",:", -1);
    for (int i = 0; tokens[i]; i++) {
        char *eq = strchr(tokens[i], '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        const char *key = tokens[i];
        const char *val = eq + 1;

        if (!strcmp(key, "thermal_max") || !strcmp(key, "thermal")) {
            s->drift.thermal_max_ppm = atoi(val);
        } else if (!strcmp(key, "tau") || !strcmp(key, "thermal_tau")) {
            s->drift.thermal_tau_ms = atoi(val);
        } else if (!strcmp(key, "linear")) {
            s->drift.linear_ppm_per_s = atoi(val);
        } else if (!strcmp(key, "ripple_amp") || !strcmp(key, "ripple")) {
            s->drift.ripple_amp_ppm = atoi(val);
        } else if (!strcmp(key, "ripple_hz") || !strcmp(key, "ripple_freq")) {
            s->drift.ripple_freq_hz = atof(val);
        } else if (!strcmp(key, "jitter")) {
            s->drift.jitter_sigma_ppm = atoi(val);
        }
    }
}

static uint64_t pic16f1_osc_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16F1SocState *s = opaque;
    if (addr < sizeof(s->osc_regs)) {
        return s->osc_regs[addr];
    }
    return 0;
}

static void pic16f1_osc_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    PIC16F1SocState *s = opaque;
    if (addr < sizeof(s->osc_regs)) {
        s->osc_regs[addr] = value;
        if (addr == 0) {
            /* OSCCON1 write updates OSCCON2 */
            s->osc_regs[1] = value;
        } else if (addr == 5) {
            /*
             * OSCTUNE register: 6-bit signed integer (TUN<5:0>), range -32 to +31.
             * Each step adjusts HFINTOSC frequency by approx 0.1% (1000 ppm).
             */
            pic16_drift_timer_cb(s);
        }
    }
}

static const MemoryRegionOps pic16f1_osc_ops = {
    .read = pic16f1_osc_read,
    .write = pic16f1_osc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static uint64_t pic16f1_pmd_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16F1SocState *s = opaque;
    if (addr < sizeof(s->pmd_regs)) {
        return s->pmd_regs[addr];
    }
    return 0;
}

static void pic16f1_pmd_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    PIC16F1SocState *s = opaque;
    if (addr < sizeof(s->pmd_regs)) {
        s->pmd_regs[addr] = value;
    }
}

static const MemoryRegionOps pic16f1_pmd_ops = {
    .read = pic16f1_pmd_read,
    .write = pic16f1_pmd_write,
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

    s->base_fosc_hz = sc->fosc_hz;
    s->drift_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pic16_drift_timer_cb, s);

    int64_t initial_hz = (int64_t)s->base_fosc_hz +
                         (int64_t)s->base_fosc_hz * s->osc_ppm / 1000000;
    if (initial_hz <= 0) {
        initial_hz = sc->fosc_hz;
    }

    s->fosc = clock_new(OBJECT(dev), "fosc");
    clock_set_hz(s->fosc, (uint64_t)initial_hz);

    if (s->drift.enabled) {
        timer_mod(s->drift_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 5 * 1000 * 1000);
    }

    /* Program flash, and the configuration words above word 0x8000. */
    memory_region_init_rom(&s->flash, OBJECT(dev), "pic16.flash",
                           sc->flash_words * 2, &error_fatal);
    memory_region_add_subregion(system_memory, OFFSET_CODE, &s->flash);

    memory_region_init_rom(&s->config, OBJECT(dev), "pic16.config",
                           PIC16_CONFIG_SIZE, &error_fatal);
    memset(memory_region_get_ram_ptr(&s->config), 0xFF, PIC16_CONFIG_SIZE);
    memory_region_add_subregion(system_memory, OFFSET_CONFIG, &s->config);

    create_unimplemented_device("pic16.sfr", OFFSET_DATA, PIC16_BANKED_SIZE);

    for (i = 0; i < sc->gpr_banks; i++) {
        g_autofree char *name = g_strdup_printf("pic16.gpr%u", i);
        unsigned size = PIC16_GPR_SIZE;

        if (i == sc->gpr_banks - 1 && sc->gpr_last_bank_size > 0) {
            size = sc->gpr_last_bank_size;
        }

        memory_region_init_ram(&s->gpr[i], OBJECT(dev), name,
                               size, &error_fatal);
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
                                OFFSET_DATA + sc->pir_addr, &s->intc);

    memory_region_init_io(&s->pcon, OBJECT(dev), &pic16f1_pcon_ops, s,
                          "pic16.pcon", PIC16_PCON_SIZE);
    memory_region_add_subregion(system_memory,
                                OFFSET_DATA + sc->pcon_addr, &s->pcon);

    memory_region_init_io(&s->core_sfr, OBJECT(dev), &pic16f1_core_sfr_ops, s,
                          "pic16.core-sfr", PIC16_CORE_SFR_SIZE);
    memory_region_add_subregion(system_memory,
                                OFFSET_DATA + sc->core_sfr_addr,
                                &s->core_sfr);

    if (sc->pps_out_addr != 0) {
        memory_region_init_io(&s->pps_out, OBJECT(dev), &pic16f1_pps_out_ops, s,
                              "pic16.pps-out", sc->pps_out_size);
        memory_region_add_subregion(system_memory,
                                    OFFSET_DATA + sc->pps_out_addr,
                                    &s->pps_out);
    }

    if (sc->pps_in_addr != 0) {
        memory_region_init_io(&s->pps_in, OBJECT(dev), &pic16f1_pps_in_ops, s,
                              "pic16.pps-in", sc->pps_in_size);
        memory_region_add_subregion(system_memory,
                                    OFFSET_DATA + sc->pps_in_addr, &s->pps_in);
    }

    if (sc->osc_addr != 0) {
        memory_region_init_io(&s->osc, OBJECT(dev), &pic16f1_osc_ops, s,
                              "pic16.osc", 8);
        memory_region_add_subregion(system_memory,
                                    OFFSET_DATA + sc->osc_addr, &s->osc);
    }

    if (sc->pmd_addr != 0) {
        memory_region_init_io(&s->pmd, OBJECT(dev), &pic16f1_pmd_ops, s,
                              "pic16.pmd", 6);
        memory_region_add_subregion(system_memory,
                                    OFFSET_DATA + sc->pmd_addr, &s->pmd);
    }

    s->cpu_irq = qdev_get_gpio_in(DEVICE(&s->cpu), 0);
    qdev_init_gpio_in_named(dev, pic16f1_soc_set_irq, PIC16_IRQ_GPIO,
                            PIC16_NUM_IRQ);
    qdev_init_gpio_in_named(dev, pic16f1_soc_set_irq_level,
                            PIC16_IRQ_LEVEL_GPIO, PIC16_NUM_IRQ);

    /* Ports */
    object_initialize_child(OBJECT(dev), "port", &s->port, TYPE_PIC16_PORT);
    s->port.layout = sc->port_layout;
    sysbus_realize(SYS_BUS_DEVICE(&s->port), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->port), 0,
                    OFFSET_DATA + sc->port_data_addr);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->port), 1,
                    OFFSET_DATA + sc->port_pad_addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->port), 0,
                       qdev_get_gpio_in_named(dev, PIC16_IRQ_LEVEL_GPIO,
                                              sc->irq_ioc));

    /* EUSART1 */
    if (sc->eusart1_addr != 0) {
        object_initialize_child(OBJECT(dev), "eusart1", &s->eusart1,
                                TYPE_PIC16_EUSART);
        s->eusart1.fosc = s->fosc;
        if (!sc->serial0_mssp1) {
            qdev_prop_set_chr(DEVICE(&s->eusart1), "chardev", serial_hd(0));
        }
        sysbus_realize(SYS_BUS_DEVICE(&s->eusart1), &error_abort);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->eusart1), 0,
                        OFFSET_DATA + sc->eusart1_addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->eusart1), 0,
                           qdev_get_gpio_in_named(dev, PIC16_IRQ_LEVEL_GPIO,
                                                  sc->irq_tx1));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->eusart1), 1,
                           qdev_get_gpio_in_named(dev, PIC16_IRQ_LEVEL_GPIO,
                                                  sc->irq_rc1));
    }

    /* MSSP1 */
    if (sc->mssp1_addr != 0) {
        object_initialize_child(OBJECT(dev), "mssp1", &s->mssp1,
                                TYPE_PIC16_MSSP);
        if ((sc->eusart1_addr == 0 || sc->serial0_mssp1) && serial_hd(0)) {
            qdev_prop_set_chr(DEVICE(&s->mssp1), "chardev", serial_hd(0));
        }
        sysbus_realize(SYS_BUS_DEVICE(&s->mssp1), &error_abort);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->mssp1), 0,
                        OFFSET_DATA + sc->mssp1_addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->mssp1), 0,
                           qdev_get_gpio_in_named(dev, PIC16_IRQ_GPIO,
                                                  sc->irq_ssp1));
    }

    /* MSSP2 */
    if (sc->mssp2_addr != 0) {
        object_initialize_child(OBJECT(dev), "mssp2", &s->mssp2,
                                TYPE_PIC16_MSSP);
        sysbus_realize(SYS_BUS_DEVICE(&s->mssp2), &error_abort);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->mssp2), 0,
                        OFFSET_DATA + sc->mssp2_addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->mssp2), 0,
                           qdev_get_gpio_in_named(dev, PIC16_IRQ_GPIO,
                                                  sc->irq_ssp2));
    }

    /* TMR0 */
    if (sc->tmr0_addr != 0) {
        object_initialize_child(OBJECT(dev), "tmr0", &s->tmr0,
                                TYPE_PIC16_TMR0);
        s->tmr0.fosc = s->fosc;
        sysbus_realize(SYS_BUS_DEVICE(&s->tmr0), &error_abort);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->tmr0), 0,
                        OFFSET_DATA + sc->tmr0_addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->tmr0), 0,
                           qdev_get_gpio_in_named(dev, PIC16_IRQ_GPIO,
                                                  sc->irq_tmr0));
    }

    /* NCO1 */
    if (sc->nco1_addr != 0) {
        object_initialize_child(OBJECT(dev), "nco1", &s->nco1,
                                TYPE_PIC16_NCO);
        s->nco1.fosc = s->fosc;
        sysbus_realize(SYS_BUS_DEVICE(&s->nco1), &error_abort);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->nco1), 0,
                        OFFSET_DATA + sc->nco1_addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->nco1), 0,
                           qdev_get_gpio_in_named(dev, PIC16_IRQ_GPIO,
                                                  sc->irq_nco1));
    }

    /* WWDT */
    if (sc->wwdt_addr != 0) {
        object_initialize_child(OBJECT(dev), "wwdt", &s->wwdt, TYPE_PIC16_WWDT);
        s->wwdt.cpu = &s->cpu;
        sysbus_realize(SYS_BUS_DEVICE(&s->wwdt), &error_abort);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->wwdt), 0,
                        OFFSET_DATA + sc->wwdt_addr);
        qdev_connect_gpio_out_named(DEVICE(&s->cpu), "clrwdt", 0,
                                    qdev_get_gpio_in_named(DEVICE(&s->wwdt),
                                                           PIC16_WWDT_CLEAR_GPIO,
                                                           0));
    }

    /* TMR1 */
    if (sc->tmr1_addr != 0) {
        object_initialize_child(OBJECT(dev), "tmr1", &s->tmr1, TYPE_PIC16_TMR1);
        s->tmr1.fosc = s->fosc;
        sysbus_realize(SYS_BUS_DEVICE(&s->tmr1), &error_abort);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->tmr1), 0,
                        OFFSET_DATA + sc->tmr1_addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->tmr1), 0,
                           qdev_get_gpio_in_named(dev, PIC16_IRQ_GPIO,
                                                  sc->irq_tmr1));
    }
}

static void pic16f1_soc_reset_hold(Object *obj, ResetType type)
{
    PIC16F1SocState *s = PIC16F1_SOC(obj);

    memset(s->pir_latch, 0, sizeof(s->pir_latch));
    memset(s->pir_level, 0, sizeof(s->pir_level));
    memset(s->pie, 0, sizeof(s->pie));
    memset(s->pps_out_regs, 0, sizeof(s->pps_out_regs));
    memset(s->pps_in_regs, 0, sizeof(s->pps_in_regs));
    memset(s->osc_regs, 0, sizeof(s->osc_regs));
    memset(s->pmd_regs, 0, sizeof(s->pmd_regs));

    s->osc_regs[0] = 0x60; /* OSCCON1: HFINTOSC */
    s->osc_regs[1] = 0x60; /* OSCCON2: HFINTOSC */
    s->osc_regs[3] = 0x40; /* OSCSTAT: HFOR ready */
    s->osc_regs[5] = 0x00; /* OSCTUNE: center frequency */
    s->osc_regs[6] = 0x06; /* OSCFRQ: 32 MHz */

    if (s->fosc && s->base_fosc_hz) {
        int64_t initial_hz = (int64_t)s->base_fosc_hz +
                             (int64_t)s->base_fosc_hz * s->osc_ppm / 1000000;
        if (initial_hz > 0) {
            clock_set_hz(s->fosc, (uint64_t)initial_hz);
            pic16_tmr0_update_fosc(&s->tmr0);
        }
    }

    s->drift.accumulated_jitter = 0.0;
    if (s->drift.enabled && s->drift_timer) {
        timer_mod(s->drift_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 5 * 1000 * 1000);
    }

    s->pcon1 = 0;
    pic16f1_soc_update_irq(s);
}

static const Property pic16f1_soc_properties[] = {
    DEFINE_PROP_INT32("osc-ppm", PIC16F1SocState, osc_ppm, 0),
};

static const VMStateDescription pic16f1_soc_vmstate = {
    .name = "pic16f1-soc",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(pir_latch, PIC16F1SocState, PIC16_NUM_PIR),
        VMSTATE_UINT8_ARRAY(pir_level, PIC16F1SocState, PIC16_NUM_PIR),
        VMSTATE_UINT8_ARRAY(pie, PIC16F1SocState, PIC16_NUM_PIR),
        VMSTATE_UINT8(pcon1, PIC16F1SocState),
        VMSTATE_BOOL(por, PIC16F1SocState),
        VMSTATE_BOOL(bor, PIC16F1SocState),
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
    device_class_set_props(dc, pic16f1_soc_properties);
    rc->phases.hold = pic16f1_soc_reset_hold;
}

static void pic16f17546_class_init(ObjectClass *oc, const void *data)
{
    PIC16F1SocClass *sc = PIC16F1_SOC_CLASS(oc);

    sc->cpu_type = PIC16_CPU_TYPE_NAME("pic16f1");
    sc->flash_words = 16 * 1024;
    sc->gpr_banks = 26;
    sc->gpr_last_bank_size = 0;
    sc->fosc_hz = 32 * 1000 * 1000;
    sc->port_layout = 0;

    sc->pir_addr = 0x08C;
    sc->pcon_addr = 0x192;
    sc->core_sfr_addr = 0x1FE4;
    sc->pps_out_addr = 0x1D8C;
    sc->pps_out_size = 0x24;
    sc->pps_in_addr = 0x1E0C;
    sc->pps_in_size = 0x54;
    sc->port_data_addr = 0x00C;
    sc->port_pad_addr = 0x1E8C;
    sc->eusart1_addr = 0x70C;
    sc->mssp1_addr = 0x78C;
    sc->mssp2_addr = 0;
    sc->tmr0_addr = 0;
    sc->tmr1_addr = 0x30C;
    sc->nco1_addr = 0;
    sc->wwdt_addr = 0x18C;
    sc->osc_addr = 0;
    sc->pmd_addr = 0;

    sc->irq_ioc = PIC16_IRQ(0, 4);
    sc->irq_tmr0 = 0;
    sc->irq_ssp1 = 0;
    sc->irq_ssp2 = 0;
    sc->irq_tmr1 = PIC16_IRQ(1, 6);
    sc->irq_tx1 = PIC16_IRQ(4, 6);
    sc->irq_rc1 = PIC16_IRQ(4, 7);
    sc->irq_nco1 = 0;
}

static void pic16f15354_class_init(ObjectClass *oc, const void *data)
{
    PIC16F1SocClass *sc = PIC16F1_SOC_CLASS(oc);

    sc->cpu_type = PIC16_CPU_TYPE_NAME("pic16f1");
    sc->flash_words = 4 * 1024;
    sc->gpr_banks = 7;      /* 512 bytes: bank 6 holds only 0x320-0x32F */
    sc->gpr_last_bank_size = 16;
    sc->fosc_hz = 32 * 1000 * 1000;
    sc->port_layout = 1;

    sc->pir_addr = 0x70C;
    sc->pcon_addr = 0x813;
    sc->core_sfr_addr = 0x1FE4;
    sc->pps_out_addr = 0x1F10;
    sc->pps_out_size = 0x18;
    sc->pps_in_addr = 0x1E8F;
    sc->pps_in_size = 0x40;
    sc->port_data_addr = 0x00C;
    sc->port_pad_addr = 0x1F38;
    sc->eusart1_addr = 0x119;
    sc->serial0_mssp1 = true;
    sc->mssp1_addr = 0x18C;
    sc->mssp2_addr = 0x196;
    sc->tmr0_addr = 0x59C;
    sc->tmr1_addr = 0x20C;
    sc->nco1_addr = 0x58C;
    sc->wwdt_addr = 0x80C;
    sc->osc_addr = 0x88D;
    sc->pmd_addr = 0x796;

    sc->irq_ioc = PIC16_IRQ(0, 4);
    sc->irq_tmr0 = PIC16_IRQ(0, 5);
    sc->irq_ssp1 = PIC16_IRQ(3, 0);
    sc->irq_ssp2 = PIC16_IRQ(3, 2);
    sc->irq_tx1 = PIC16_IRQ(3, 4);
    sc->irq_rc1 = PIC16_IRQ(3, 5);
    sc->irq_tmr1 = PIC16_IRQ(4, 0);
    sc->irq_nco1 = PIC16_IRQ(7, 4);
}

/*
 * The PIC16F15355 is the same die with twice the memory: 8K words of flash and
 * 1024 bytes of SRAM, so banks 6 to 11 fill out and bank 12 gets 0x620-0x64F.
 * Every register sits where the '354 has it.
 */
static void pic16f15355_class_init(ObjectClass *oc, const void *data)
{
    PIC16F1SocClass *sc = PIC16F1_SOC_CLASS(oc);

    sc->flash_words = 8 * 1024;
    sc->gpr_banks = 13;
    sc->gpr_last_bank_size = 48;
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
    {
        .name = TYPE_PIC16F15354_SOC,
        .parent = TYPE_PIC16F1_SOC,
        .class_init = pic16f15354_class_init,
    },
    {
        .name = TYPE_PIC16F15355_SOC,
        .parent = TYPE_PIC16F15354_SOC,
        .class_init = pic16f15355_class_init,
    },
};

DEFINE_TYPES(pic16f1_soc_types)
