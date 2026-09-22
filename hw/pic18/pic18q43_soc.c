/*
 * PIC18F-Q43 SoC
 *
 * Builds the memory map and the parts of the chip that belong to the core
 * rather than to a peripheral: the interrupt fan-in (PIRx/PIEx/IPRx in legacy
 * vector mode, resolved to the CPU's high- and low-priority request lines),
 * the interrupt shadow window, the oscillator block with its tuning and drift
 * machinery, PMD, and the Peripheral Pin Select registers with live routing of
 * the SPI select outputs onto port pins.
 *
 * The core's own SFRs -- INTCON0/1, STATUS, the FSRs and INDFs, BSR, WREG,
 * PCON0/1, CPUDOZE, PROD, TABLAT, TBLPTR, PCL/PCLATH/PCLATU, STKPTR, TOS --
 * live at 0x4D6..0x4FF and are implemented inside the CPU; nothing is mapped
 * there.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/misc/unimp.h"
#include "exec/target_page.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties-system.h"
#include "system/system.h"
#include "pic18q43_soc.h"
#include <math.h>

/* PIE0..15 then PIR0..15 sit back to back at 0x49E; IPR0..15 are at 0x362. */
#define PIC18_INTC_SIZE     (2 * PIC18_NUM_PIR)
#define PIC18_PIR_OFFSET    PIC18_NUM_PIR
#define PIC18_IPR_SIZE      PIC18_NUM_PIR

#define PIC18_SHAD_SIZE     0x0E

#define PIC18_CONFIG_SIZE   0x10    /* CONFIG1..10, padded to the row */

/* RxyPPS output selections this SoC routes onto the pin. */
#define PPS_OUT_SPI1_SS     0x33
#define PPS_OUT_SPI2_SS     0x36

/* OSCFRQ.HFFRQ encodings, in MHz. */
static const unsigned pic18_hffrq_mhz[] = { 1, 2, 4, 8, 12, 16, 32, 48, 64 };

enum {
    OSC_REG_OSCCON1,
    OSC_REG_OSCCON2,
    OSC_REG_OSCCON3,
    OSC_REG_OSCTUNE,
    OSC_REG_OSCFRQ,
    OSC_REG_OSCSTAT,
    OSC_REG_OSCEN,
};

/*
 * Legacy vector mode: every enabled, pending flag with IPR set requests the
 * high-priority vector and every one with IPR clear the low-priority vector.
 * IPEN and GIE/GIEH/GIEL gate these in the CPU, where INTCON0 lives.
 */
static void pic18q43_soc_update_irq(PIC18Q43SocState *s)
{
    bool high = false, low = false;
    int i;

    for (i = 0; i < PIC18_NUM_PIR; i++) {
        uint8_t pending = (s->pir_latch[i] | s->pir_level[i]) & s->pie[i];

        if (pending & s->ipr_regs[i]) {
            high = true;
        }
        if (pending & ~s->ipr_regs[i]) {
            low = true;
        }
    }
    qemu_set_irq(s->cpu_irq_high, high);
    qemu_set_irq(s->cpu_irq_low, low);
}

/*
 * A peripheral raising its line sets the PIR flag; only software clears it
 * again, which is how the hardware behaves for every latching flag. Flags
 * that hardware holds rather than latches (the SPI FIFO flags, IOCIF) are
 * kept asserted by their peripheral instead.
 */
static void pic18q43_soc_set_irq(void *opaque, int n, int level)
{
    PIC18Q43SocState *s = opaque;

    if (level) {
        s->pir_latch[n / 8] |= 1u << (n % 8);
        pic18q43_soc_update_irq(s);
    }
}

static void pic18q43_soc_set_irq_level(void *opaque, int n, int level)
{
    PIC18Q43SocState *s = opaque;
    uint8_t mask = 1u << (n % 8);

    if (level) {
        s->pir_level[n / 8] |= mask;
    } else {
        s->pir_level[n / 8] &= ~mask;
    }
    pic18q43_soc_update_irq(s);
}

static uint64_t pic18q43_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC18Q43SocState *s = opaque;

    if (addr < PIC18_PIR_OFFSET) {
        return s->pie[addr];
    }
    if (addr < PIC18_PIR_OFFSET + PIC18_NUM_PIR) {
        addr -= PIC18_PIR_OFFSET;
        return s->pir_latch[addr] | s->pir_level[addr];
    }
    return 0;
}

static void pic18q43_intc_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    PIC18Q43SocState *s = opaque;

    if (addr < PIC18_PIR_OFFSET) {
        s->pie[addr] = value;
    } else if (addr < PIC18_PIR_OFFSET + PIC18_NUM_PIR) {
        addr -= PIC18_PIR_OFFSET;
        /* Only the latched flags are software-writable. */
        s->pir_latch[addr] = value & ~s->pir_level[addr];
    } else {
        return;
    }
    pic18q43_soc_update_irq(s);
}

static const MemoryRegionOps pic18q43_intc_ops = {
    .read = pic18q43_intc_read,
    .write = pic18q43_intc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static uint64_t pic18q43_ipr_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC18Q43SocState *s = opaque;

    return addr < PIC18_NUM_PIR ? s->ipr_regs[addr] : 0;
}

static void pic18q43_ipr_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    PIC18Q43SocState *s = opaque;

    if (addr < PIC18_NUM_PIR) {
        s->ipr_regs[addr] = value;
        pic18q43_soc_update_irq(s);
    }
}

static const MemoryRegionOps pic18q43_ipr_ops = {
    .read = pic18q43_ipr_read,
    .write = pic18q43_ipr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/*
 * SHADCON and the interrupt shadow registers, 0x376..0x383. The CPU keeps
 * both contexts; SHADCON.SHADLO says which one the window shows (1 the main
 * context, 0 the low-priority ISR context, per the data sheet).
 */
static uint64_t pic18q43_shad_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC18Q43SocState *s = opaque;

    if (addr == 0) {
        return s->shadcon;
    }
    if (addr - 1 < PIC18_NUM_SHADOW) {
        return pic18_shadow_read(&s->cpu.env, addr - 1, !(s->shadcon & 1));
    }
    return 0;
}

static void pic18q43_shad_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    PIC18Q43SocState *s = opaque;

    if (addr == 0) {
        s->shadcon = value & 0x01;
    } else if (addr - 1 < PIC18_NUM_SHADOW) {
        pic18_shadow_write(&s->cpu.env, addr - 1, !(s->shadcon & 1), value);
    }
}

static const MemoryRegionOps pic18q43_shad_ops = {
    .read = pic18q43_shad_read,
    .write = pic18q43_shad_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/*
 * Peripheral Pin Select, output side. Every RxyPPS is stored; the ones this
 * model acts on are the SPI select outputs, because the pin then follows the
 * module's SS(out) instead of its latch and the firmware relies on that for
 * the AD9834's FSYNC. Writing RxyPPS re-evaluates the pin at once.
 */
static void pic18q43_soc_route_pin(PIC18Q43SocState *s, unsigned line)
{
    uint8_t code = s->pps_out_regs[1 + line];

    switch (code) {
    case PPS_OUT_SPI1_SS:
        pic18_port_set_peripheral_output(&s->port, line, true,
                                         s->spi_ss_level[0]);
        break;
    case PPS_OUT_SPI2_SS:
        pic18_port_set_peripheral_output(&s->port, line, true,
                                         s->spi_ss_level[1]);
        break;
    default:
        /* Other peripheral outputs (SCK, SDO, ...) are not pin-modelled. */
        pic18_port_set_peripheral_output(&s->port, line, false, false);
        break;
    }
}

/* An SPI module's SS(out) changed: every pin routed to it follows. */
static void pic18q43_soc_spi_ss(void *opaque, int n, int level)
{
    PIC18Q43SocState *s = opaque;
    uint8_t code = n == 0 ? PPS_OUT_SPI1_SS : PPS_OUT_SPI2_SS;
    unsigned line;

    s->spi_ss_level[n] = level != 0;
    for (line = 0; line < PIC18_PPS_OUT_LINES; line++) {
        if (s->pps_out_regs[1 + line] == code) {
            pic18q43_soc_route_pin(s, line);
        }
    }
}

static uint64_t pic18q43_pps_out_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC18Q43SocState *s = opaque;

    return addr < sizeof(s->pps_out_regs) ? s->pps_out_regs[addr] : 0;
}

static void pic18q43_pps_out_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    PIC18Q43SocState *s = opaque;

    if (addr >= sizeof(s->pps_out_regs)) {
        return;
    }
    if (addr == 0) {
        /* PPSLOCK: the unlock sequence is not enforced. */
        s->pps_out_regs[0] = value & 0x01;
        return;
    }
    s->pps_out_regs[addr] = value & 0x3F;
    pic18q43_soc_route_pin(s, addr - 1);
}

static const MemoryRegionOps pic18q43_pps_out_ops = {
    .read = pic18q43_pps_out_read,
    .write = pic18q43_pps_out_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static uint64_t pic18q43_pps_in_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC18Q43SocState *s = opaque;

    return addr < sizeof(s->pps_in_regs) ? s->pps_in_regs[addr] : 0;
}

static void pic18q43_pps_in_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    PIC18Q43SocState *s = opaque;

    if (addr < sizeof(s->pps_in_regs)) {
        s->pps_in_regs[addr] = value & 0x1F;
    }
}

static const MemoryRegionOps pic18q43_pps_in_ops = {
    .read = pic18q43_pps_in_read,
    .write = pic18q43_pps_in_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/*
 * The oscillator. HFINTOSC runs at the OSCFRQ frequency, trimmed by OSCTUNE
 * (TUN<5:0>, two's complement, about 1000 ppm a step), offset by the board's
 * osc-ppm and by whatever drift profile is running. The result is the fosc
 * Clock the peripherals count from; the CPU's own pace is -icount's business.
 */
static void pic18_drift_timer_cb(void *opaque)
{
    PIC18Q43SocState *s = opaque;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    double t_sec = (double)now_ns * 1e-9;
    double thermal_ppm = 0.0;
    double linear_ppm, ripple_ppm = 0.0;
    int8_t tune;
    int32_t dynamic_drift_ppm, total_ppm;
    int64_t tuned_hz;

    if (s->drift.thermal_tau_ms > 0) {
        double tau_sec = (double)s->drift.thermal_tau_ms * 1e-3;

        thermal_ppm = (double)s->drift.thermal_max_ppm *
                      (1.0 - exp(-t_sec / tau_sec));
    }

    linear_ppm = (double)s->drift.linear_ppm_per_s * t_sec;
    if (s->drift.ripple_amp_ppm != 0 && s->drift.ripple_freq_hz > 0.0) {
        ripple_ppm = (double)s->drift.ripple_amp_ppm *
                     sin(2.0 * M_PI * s->drift.ripple_freq_hz * t_sec);
    }

    if (s->drift.jitter_sigma_ppm > 0) {
        double step = ((double)rand() / (double)RAND_MAX) - 0.5;

        s->drift.accumulated_jitter +=
            step * (double)s->drift.jitter_sigma_ppm * 0.2;
        if (s->drift.accumulated_jitter > 3.0 * s->drift.jitter_sigma_ppm) {
            s->drift.accumulated_jitter = 3.0 * s->drift.jitter_sigma_ppm;
        } else if (s->drift.accumulated_jitter <
                   -3.0 * s->drift.jitter_sigma_ppm) {
            s->drift.accumulated_jitter = -3.0 * s->drift.jitter_sigma_ppm;
        }
    }

    tune = (int8_t)((s->osc_regs[OSC_REG_OSCTUNE] & 0x3F) |
                    ((s->osc_regs[OSC_REG_OSCTUNE] & 0x20) ? 0xC0 : 0));
    dynamic_drift_ppm = (int32_t)(thermal_ppm + linear_ppm + ripple_ppm +
                                  s->drift.accumulated_jitter);
    total_ppm = s->osc_ppm + (int32_t)tune * 1000 + dynamic_drift_ppm;

    tuned_hz = (int64_t)s->base_fosc_hz +
               (int64_t)s->base_fosc_hz * total_ppm / 1000000;
    if (tuned_hz > 0 && s->fosc) {
        clock_set_hz(s->fosc, (uint64_t)tuned_hz);
        pic18_tmr0_update_fosc(&s->tmr0);
    }

    if (s->drift.enabled && s->drift_timer) {
        timer_mod(s->drift_timer, now_ns + 5 * 1000 * 1000); /* 5 ms */
    }
}

void pic18q43_soc_parse_drift(PIC18Q43SocState *s, const char *str)
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
        const char *key, *val;

        if (!eq) {
            continue;
        }
        *eq = '\0';
        key = tokens[i];
        val = eq + 1;

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

static void pic18q43_soc_set_hffrq(PIC18Q43SocState *s, uint8_t code)
{
    if (code < ARRAY_SIZE(pic18_hffrq_mhz)) {
        s->base_fosc_hz = (uint64_t)pic18_hffrq_mhz[code] * 1000 * 1000;
    }
}

static uint64_t pic18q43_osc_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC18Q43SocState *s = opaque;

    return addr < PIC18_OSC_NREGS ? s->osc_regs[addr] : 0;
}

static void pic18q43_osc_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    PIC18Q43SocState *s = opaque;

    if (addr >= PIC18_OSC_NREGS) {
        return;
    }
    switch (addr) {
    case OSC_REG_OSCCON1:
        /* A clock switch completes at once: OSCCON2 shows the new source. */
        s->osc_regs[OSC_REG_OSCCON1] = value & 0x7F;
        s->osc_regs[OSC_REG_OSCCON2] = value & 0x7F;
        break;
    case OSC_REG_OSCCON2:
    case OSC_REG_OSCSTAT:
        break;      /* read only */
    case OSC_REG_OSCTUNE:
        s->osc_regs[OSC_REG_OSCTUNE] = value & 0x3F;
        pic18_drift_timer_cb(s);
        break;
    case OSC_REG_OSCFRQ:
        s->osc_regs[OSC_REG_OSCFRQ] = value & 0x0F;
        pic18q43_soc_set_hffrq(s, value & 0x0F);
        pic18_drift_timer_cb(s);
        break;
    default:
        s->osc_regs[addr] = value;
        break;
    }
}

static const MemoryRegionOps pic18q43_osc_ops = {
    .read = pic18q43_osc_read,
    .write = pic18q43_osc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static uint64_t pic18q43_pmd_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC18Q43SocState *s = opaque;

    return addr < PIC18_PMD_NREGS ? s->pmd_regs[addr] : 0;
}

static void pic18q43_pmd_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    PIC18Q43SocState *s = opaque;

    if (addr < PIC18_PMD_NREGS) {
        s->pmd_regs[addr] = value;
    }
}

static const MemoryRegionOps pic18q43_pmd_ops = {
    .read = pic18q43_pmd_read,
    .write = pic18q43_pmd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/*
 * IVTLOCK and IVTBASE (0x458..0x45F). XC8's start-up code writes IVTBASE even
 * with MVECEN = 0; vectored interrupts are not modelled, so this is storage.
 */
static uint64_t pic18q43_ivt_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC18Q43SocState *s = opaque;

    return addr < PIC18_IVT_NREGS ? s->ivt_regs[addr] : 0;
}

static void pic18q43_ivt_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    PIC18Q43SocState *s = opaque;

    if (addr < PIC18_IVT_NREGS) {
        s->ivt_regs[addr] = value;
    }
}

static const MemoryRegionOps pic18q43_ivt_ops = {
    .read = pic18q43_ivt_read,
    .write = pic18q43_ivt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic18q43_soc_init_rom(PIC18Q43SocState *s, MemoryRegion *mr,
                                  const char *name, hwaddr base,
                                  uint64_t size)
{
    memory_region_init_rom(mr, OBJECT(s), name, size, &error_fatal);
    memset(memory_region_get_ram_ptr(mr), 0xFF, size);
    memory_region_add_subregion(get_system_memory(), OFFSET_CODE + base, mr);
}

static void pic18q43_soc_connect_spi(PIC18Q43SocState *s, PIC18SpiState *spi,
                                     const char *name, hwaddr addr,
                                     int irq_rx, int irq_tx, int irq_gen,
                                     int ss_index)
{
    DeviceState *dev = DEVICE(s);

    object_initialize_child(OBJECT(dev), name, spi, TYPE_PIC18_SPI);
    if (ss_index == 0 && serial_hd(0)) {
        /* Serial 0 is the byte stream from whatever masters SPI1. */
        qdev_prop_set_chr(DEVICE(spi), "chardev", serial_hd(0));
    }
    sysbus_realize(SYS_BUS_DEVICE(spi), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(spi), 0, OFFSET_DATA + addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(spi), PIC18_SPI_IRQ_RX,
                       qdev_get_gpio_in_named(dev, PIC18_IRQ_LEVEL_GPIO,
                                              irq_rx));
    sysbus_connect_irq(SYS_BUS_DEVICE(spi), PIC18_SPI_IRQ_TX,
                       qdev_get_gpio_in_named(dev, PIC18_IRQ_LEVEL_GPIO,
                                              irq_tx));
    sysbus_connect_irq(SYS_BUS_DEVICE(spi), PIC18_SPI_IRQ_GEN,
                       qdev_get_gpio_in_named(dev, PIC18_IRQ_LEVEL_GPIO,
                                              irq_gen));
    qdev_connect_gpio_out_named(DEVICE(spi), PIC18_SPI_SS_OUT_GPIO, 0,
                                qdev_get_gpio_in_named(dev,
                                                       PIC18_SOC_SPI_SS_GPIO,
                                                       ss_index));
}

static void pic18q43_soc_realize(DeviceState *dev, Error **errp)
{
    PIC18Q43SocState *s = PIC18Q43_SOC(dev);
    PIC18Q43SocClass *sc = PIC18Q43_SOC_GET_CLASS(dev);
    MemoryRegion *system_memory = get_system_memory();
    int64_t initial_hz;

    object_initialize_child(OBJECT(dev), "cpu", &s->cpu, sc->cpu_type);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_abort);

    s->base_fosc_hz = sc->fosc_hz;
    s->drift_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pic18_drift_timer_cb, s);

    initial_hz = (int64_t)s->base_fosc_hz +
                 (int64_t)s->base_fosc_hz * s->osc_ppm / 1000000;
    if (initial_hz <= 0) {
        initial_hz = sc->fosc_hz;
    }

    s->fosc = clock_new(OBJECT(dev), "fosc");
    clock_set_hz(s->fosc, (uint64_t)initial_hz);

    if (s->drift.enabled) {
        timer_mod(s->drift_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 5 * 1000 * 1000);
    }

    /* Program space: flash, user IDs, configuration bytes, data EEPROM. */
    pic18q43_soc_init_rom(s, &s->flash, "pic18.flash", 0, sc->flash_size);
    pic18q43_soc_init_rom(s, &s->userid, "pic18.userid", PIC18_USERID_BASE,
                          PIC18_USERID_SIZE);
    pic18q43_soc_init_rom(s, &s->config, "pic18.config", PIC18_CONFIG_BASE,
                          PIC18_CONFIG_SIZE);
    pic18q43_soc_init_rom(s, &s->eeprom, "pic18.eeprom", PIC18_EEPROM_BASE,
                          PIC18_EEPROM_SIZE);

    /*
     * Data space: the SFR window below 0x500, with a catcher under the
     * modelled devices so an unmodelled register shows up under -d unimp
     * rather than reading 0 silently -- stopping short of the core's own
     * SFRs, which the CPU implements -- then the GPR RAM.
     */
    create_unimplemented_device("pic18.sfr", OFFSET_DATA, PIC18_CORE_SFR_BASE);

    /*
     * Split at the first page boundary: a RAM region that starts inside a
     * page but runs on past it trips the softmmu (the RAM offset must be
     * page-aligned for a page it fully covers). The first 0x300 bytes share
     * their page with the SFRs and take the subpage path; the rest is
     * ordinary fast RAM.
     */
    {
        hwaddr split = QEMU_ALIGN_UP(PIC18_SFR_END, TARGET_PAGE_SIZE);
        unsigned lo_size = split - PIC18_SFR_END;

        assert(sc->gpr_size > lo_size);
        memory_region_init_ram(&s->gpr_lo, OBJECT(dev), "pic18.gpr-lo",
                               lo_size, &error_fatal);
        memory_region_add_subregion(system_memory, OFFSET_DATA + PIC18_SFR_END,
                                    &s->gpr_lo);
        memory_region_init_ram(&s->gpr, OBJECT(dev), "pic18.gpr",
                               sc->gpr_size - lo_size, &error_fatal);
        memory_region_add_subregion(system_memory, OFFSET_DATA + split,
                                    &s->gpr);
    }

    memory_region_init_io(&s->intc, OBJECT(dev), &pic18q43_intc_ops, s,
                          "pic18.intc", PIC18_INTC_SIZE);
    memory_region_add_subregion(system_memory, OFFSET_DATA + sc->pie_addr,
                                &s->intc);
    assert(sc->pir_addr == sc->pie_addr + PIC18_PIR_OFFSET);

    memory_region_init_io(&s->ipr, OBJECT(dev), &pic18q43_ipr_ops, s,
                          "pic18.ipr", PIC18_IPR_SIZE);
    memory_region_add_subregion(system_memory, OFFSET_DATA + sc->ipr_addr,
                                &s->ipr);

    memory_region_init_io(&s->shad, OBJECT(dev), &pic18q43_shad_ops, s,
                          "pic18.shadow", PIC18_SHAD_SIZE);
    memory_region_add_subregion(system_memory, OFFSET_DATA + sc->shad_addr,
                                &s->shad);

    memory_region_init_io(&s->pps_out, OBJECT(dev), &pic18q43_pps_out_ops, s,
                          "pic18.pps-out", PIC18_PPS_OUT_SIZE);
    memory_region_add_subregion(system_memory, OFFSET_DATA + sc->pps_out_addr,
                                &s->pps_out);

    memory_region_init_io(&s->pps_in, OBJECT(dev), &pic18q43_pps_in_ops, s,
                          "pic18.pps-in", PIC18_PPS_IN_SIZE);
    memory_region_add_subregion(system_memory, OFFSET_DATA + sc->pps_in_addr,
                                &s->pps_in);

    memory_region_init_io(&s->osc, OBJECT(dev), &pic18q43_osc_ops, s,
                          "pic18.osc", PIC18_OSC_NREGS);
    memory_region_add_subregion(system_memory, OFFSET_DATA + sc->osc_addr,
                                &s->osc);

    memory_region_init_io(&s->pmd, OBJECT(dev), &pic18q43_pmd_ops, s,
                          "pic18.pmd", PIC18_PMD_NREGS);
    memory_region_add_subregion(system_memory, OFFSET_DATA + sc->pmd_addr,
                                &s->pmd);

    memory_region_init_io(&s->ivt, OBJECT(dev), &pic18q43_ivt_ops, s,
                          "pic18.ivt", PIC18_IVT_NREGS);
    memory_region_add_subregion(system_memory, OFFSET_DATA + sc->ivt_addr,
                                &s->ivt);

    s->cpu_irq_high = qdev_get_gpio_in(DEVICE(&s->cpu), PIC18_IRQ_HIGH);
    s->cpu_irq_low = qdev_get_gpio_in(DEVICE(&s->cpu), PIC18_IRQ_LOW);
    qdev_init_gpio_in_named(dev, pic18q43_soc_set_irq, PIC18_IRQ_GPIO,
                            PIC18_NUM_IRQ);
    qdev_init_gpio_in_named(dev, pic18q43_soc_set_irq_level,
                            PIC18_IRQ_LEVEL_GPIO, PIC18_NUM_IRQ);
    qdev_init_gpio_in_named(dev, pic18q43_soc_spi_ss, PIC18_SOC_SPI_SS_GPIO,
                            2);

    /* Ports */
    object_initialize_child(OBJECT(dev), "port", &s->port, TYPE_PIC18_PORT);
    sysbus_realize(SYS_BUS_DEVICE(&s->port), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->port), 0,
                    OFFSET_DATA + sc->port_data_addr);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->port), 1,
                    OFFSET_DATA + sc->port_pad_addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->port), 0,
                       qdev_get_gpio_in_named(dev, PIC18_IRQ_LEVEL_GPIO,
                                              sc->irq_ioc));

    /* SPI1 (the host link, a client) and SPI2 (the DDS and PLL, a host). */
    pic18q43_soc_connect_spi(s, &s->spi1, "spi1", sc->spi1_addr,
                             sc->irq_spi1_rx, sc->irq_spi1_tx, sc->irq_spi1,
                             0);
    pic18q43_soc_connect_spi(s, &s->spi2, "spi2", sc->spi2_addr,
                             sc->irq_spi2_rx, sc->irq_spi2_tx, sc->irq_spi2,
                             1);

    /* TMR0 */
    object_initialize_child(OBJECT(dev), "tmr0", &s->tmr0, TYPE_PIC18_TMR0);
    s->tmr0.fosc = s->fosc;
    sysbus_realize(SYS_BUS_DEVICE(&s->tmr0), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tmr0), 0, OFFSET_DATA + sc->tmr0_addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->tmr0), 0,
                       qdev_get_gpio_in_named(dev, PIC18_IRQ_GPIO,
                                              sc->irq_tmr0));

    /* WWDT */
    object_initialize_child(OBJECT(dev), "wwdt", &s->wwdt, TYPE_PIC18_WWDT);
    s->wwdt.cpu = &s->cpu;
    sysbus_realize(SYS_BUS_DEVICE(&s->wwdt), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->wwdt), 0, OFFSET_DATA + sc->wwdt_addr);
    qdev_connect_gpio_out_named(DEVICE(&s->cpu), "clrwdt", 0,
                                qdev_get_gpio_in_named(DEVICE(&s->wwdt),
                                                       PIC18_WWDT_CLEAR_GPIO,
                                                       0));
}

static void pic18q43_soc_reset_hold(Object *obj, ResetType type)
{
    PIC18Q43SocState *s = PIC18Q43_SOC(obj);
    PIC18Q43SocClass *sc = PIC18Q43_SOC_GET_CLASS(obj);
    unsigned line;

    memset(s->pir_latch, 0, sizeof(s->pir_latch));
    memset(s->pir_level, 0, sizeof(s->pir_level));
    memset(s->pie, 0, sizeof(s->pie));
    memset(s->ipr_regs, 0xFF, sizeof(s->ipr_regs));  /* all high priority */
    memset(s->pps_out_regs, 0, sizeof(s->pps_out_regs));
    memset(s->pps_in_regs, 0, sizeof(s->pps_in_regs));
    memset(s->osc_regs, 0, sizeof(s->osc_regs));
    memset(s->pmd_regs, 0, sizeof(s->pmd_regs));
    memset(s->ivt_regs, 0, sizeof(s->ivt_regs));
    s->ivt_regs[5] = 0x08;  /* IVTBASE resets to 0x000008 */
    s->shadcon = 0;

    /* Every RxyPPS is 0 again: the pins are back on their latches. */
    for (line = 0; line < PIC18_PPS_OUT_LINES; line++) {
        pic18q43_soc_route_pin(s, line);
    }

    /* RSTOSC = HFINTOSC_64MHZ: OSCCON1/2 NOSC = 110, HFFRQ = 64 MHz. */
    s->osc_regs[OSC_REG_OSCCON1] = 0x60;
    s->osc_regs[OSC_REG_OSCCON2] = 0x60;
    s->osc_regs[OSC_REG_OSCCON3] = 0x00;
    s->osc_regs[OSC_REG_OSCTUNE] = 0x00;
    s->osc_regs[OSC_REG_OSCFRQ] = 0x08;
    s->osc_regs[OSC_REG_OSCSTAT] = 0x40;  /* HFOR: HFINTOSC ready */
    s->osc_regs[OSC_REG_OSCEN] = 0x00;
    s->base_fosc_hz = sc->fosc_hz;

    if (s->fosc && s->base_fosc_hz) {
        int64_t initial_hz = (int64_t)s->base_fosc_hz +
                             (int64_t)s->base_fosc_hz * s->osc_ppm / 1000000;

        if (initial_hz > 0) {
            clock_set_hz(s->fosc, (uint64_t)initial_hz);
            pic18_tmr0_update_fosc(&s->tmr0);
        }
    }

    s->drift.accumulated_jitter = 0.0;
    if (s->drift.enabled && s->drift_timer) {
        timer_mod(s->drift_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 5 * 1000 * 1000);
    }

    pic18q43_soc_update_irq(s);
}

static const Property pic18q43_soc_properties[] = {
    DEFINE_PROP_INT32("osc-ppm", PIC18Q43SocState, osc_ppm, 0),
};

static const VMStateDescription pic18q43_soc_vmstate = {
    .name = "pic18q43-soc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(pir_latch, PIC18Q43SocState, PIC18_NUM_PIR),
        VMSTATE_UINT8_ARRAY(pir_level, PIC18Q43SocState, PIC18_NUM_PIR),
        VMSTATE_UINT8_ARRAY(pie, PIC18Q43SocState, PIC18_NUM_PIR),
        VMSTATE_UINT8_ARRAY(ipr_regs, PIC18Q43SocState, PIC18_NUM_PIR),
        VMSTATE_UINT8_ARRAY(pps_out_regs, PIC18Q43SocState,
                            PIC18_PPS_OUT_SIZE),
        VMSTATE_UINT8_ARRAY(pps_in_regs, PIC18Q43SocState, PIC18_PPS_IN_SIZE),
        VMSTATE_UINT8_ARRAY(osc_regs, PIC18Q43SocState, PIC18_OSC_NREGS),
        VMSTATE_UINT8_ARRAY(pmd_regs, PIC18Q43SocState, PIC18_PMD_NREGS),
        VMSTATE_UINT8_ARRAY(ivt_regs, PIC18Q43SocState, PIC18_IVT_NREGS),
        VMSTATE_UINT8(shadcon, PIC18Q43SocState),
        VMSTATE_BOOL_ARRAY(spi_ss_level, PIC18Q43SocState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void pic18q43_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic18q43_soc_realize;
    dc->vmsd = &pic18q43_soc_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, pic18q43_soc_properties);
    rc->phases.hold = pic18q43_soc_reset_hold;
}

/*
 * PIC18F27Q43: 128 KB flash, 8 KB SRAM, 28 pins. Register addresses from the
 * data sheet's SFR map (DS40002147F) and the XC8 device header.
 */
static void pic18f27q43_class_init(ObjectClass *oc, const void *data)
{
    PIC18Q43SocClass *sc = PIC18Q43_SOC_CLASS(oc);

    sc->cpu_type = PIC18_CPU_TYPE_NAME("pic18q43");
    sc->flash_size = PIC18_FLASH_SIZE;
    sc->gpr_size = 0x2000;
    sc->fosc_hz = 64 * 1000 * 1000;

    sc->pie_addr = 0x49E;
    sc->pir_addr = 0x4AE;
    sc->ipr_addr = 0x362;
    sc->shad_addr = 0x376;
    sc->osc_addr = 0x0AD;
    sc->pmd_addr = 0x060;
    sc->ivt_addr = 0x458;
    sc->wwdt_addr = 0x078;
    sc->tmr0_addr = 0x318;
    sc->port_data_addr = 0x4BE;
    sc->port_pad_addr = 0x400;
    sc->pps_out_addr = 0x200;
    sc->pps_in_addr = 0x23E;
    sc->spi1_addr = 0x080;
    sc->spi2_addr = 0x08D;

    sc->irq_ioc = PIC18_IRQ(0, 7);
    sc->irq_tmr0 = PIC18_IRQ(3, 7);
    sc->irq_spi1_rx = PIC18_IRQ(3, 0);
    sc->irq_spi1_tx = PIC18_IRQ(3, 1);
    sc->irq_spi1 = PIC18_IRQ(3, 2);
    sc->irq_spi2_rx = PIC18_IRQ(5, 0);
    sc->irq_spi2_tx = PIC18_IRQ(5, 1);
    sc->irq_spi2 = PIC18_IRQ(5, 2);
}

static const TypeInfo pic18q43_soc_types[] = {
    {
        .name = TYPE_PIC18Q43_SOC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC18Q43SocState),
        .class_size = sizeof(PIC18Q43SocClass),
        .class_init = pic18q43_soc_class_init,
        .abstract = true,
    },
    {
        .name = TYPE_PIC18F27Q43_SOC,
        .parent = TYPE_PIC18Q43_SOC,
        .class_init = pic18f27q43_class_init,
    },
};

DEFINE_TYPES(pic18q43_soc_types)
