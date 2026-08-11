/*
 * PIC32MK SoC
 *
 * The memory map and the parts of the chip that belong to the core rather than
 * to a peripheral. Which peripherals are fitted and how big the memories are
 * come from the class, so a second part in the family is a table rather than a
 * file.
 *
 * Everything in the SFR window that is not claimed by a device is an
 * unimplemented region, so an access to a peripheral this machine does not
 * model is a log line naming the address instead of a zero the guest quietly
 * believes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "hw/pic32/pic32_regs.h"
#include "qapi/error.h"
#include "system/system.h"
#include "pic32mk_soc.h"

/*
 * The watchdog ran out. The reset it asks for clears every register including
 * the one that says why, so the cause is left with the module that owns it
 * before the reset arrives.
 */
static void pic32mk_soc_wdt_timeout(void *opaque, int line, int level)
{
    PIC32MKSocState *s = opaque;

    if (level) {
        pic32_cru_set_reset_cause(&s->cru, PIC32_RCON_WDTO);
    }
}

void pic32mk_soc_set_watchdog(PIC32MKSocState *s, bool enabled)
{
    qdev_prop_set_bit(DEVICE(&s->wdt), "enabled", enabled);
}

/* A held line into the interrupt controller, for a source that drives one. */
static qemu_irq pic32_soc_irq_level(PIC32MKSocState *s, unsigned source)
{
    return qdev_get_gpio_in_named(DEVICE(&s->evic),
                                  PIC32_EVIC_IRQ_LEVEL_GPIO, source);
}

/*
 * Keeps the module-disable flags agreeing with the PMD registers, and tells
 * the pin select block whether IOLOCK is set. Registered with the CRU, which
 * calls it however the registers change -- a guest write, a reset, an
 * incoming migration. The bit positions are registers 40-1 to 40-7 of the
 * data sheet: timers from PMD4<8:0>, UARTs from PMD5<5:0>, SPI from
 * PMD5<13:8>, the parallel port at PMD6<16> and the DMA controller -- its
 * CRC engine included -- at PMD7<4>.
 */
static void pic32mk_soc_cfg_changed(void *opaque)
{
    PIC32MKSocState *s = opaque;
    unsigned i;

    for (i = 0; i < PIC32_NUM_TIMERS; i++) {
        s->pmd_gate[PIC32_PMD_GATE_TIMER1 + i] = (s->cru.pmd[3] >> i) & 1;
    }
    for (i = 0; i < PIC32_NUM_UARTS; i++) {
        s->pmd_gate[PIC32_PMD_GATE_UART1 + i] = (s->cru.pmd[4] >> i) & 1;
    }
    for (i = 0; i < PIC32_NUM_SPIS; i++) {
        s->pmd_gate[PIC32_PMD_GATE_SPI1 + i] =
            (s->cru.pmd[4] >> (7 + pic32_spi_number(i))) & 1;
    }
    s->pmd_gate[PIC32_PMD_GATE_PMP] = (s->cru.pmd[5] >> 16) & 1;
    s->pmd_gate[PIC32_PMD_GATE_DMAC] = (s->cru.pmd[6] >> 4) & 1;
}

/* Which controller each entry of the spi[] array is. */
static const unsigned pic32_spi_numbers[PIC32_NUM_SPIS] = { 1, 3, 4 };

unsigned pic32_spi_number(unsigned index)
{
    assert(index < PIC32_NUM_SPIS);
    return pic32_spi_numbers[index];
}

static void pic32mk_soc_init(Object *obj)
{
    PIC32MKSocState *s = PIC32MK_SOC(obj);
    unsigned i;

    s->sysclk = qdev_init_clock_out(DEVICE(obj), "sysclk");
    s->pbclk = qdev_init_clock_out(DEVICE(obj), "pbclk");

    object_initialize_child(obj, "evic", &s->evic, TYPE_PIC32_EVIC);
    object_initialize_child(obj, "cru", &s->cru, TYPE_PIC32_CRU);
    object_initialize_child(obj, "pps", &s->pps, TYPE_PIC32_PPS);
    object_initialize_child(obj, "pmp", &s->pmp, TYPE_PIC32_PMP);
    object_initialize_child(obj, "dmac", &s->dmac, TYPE_PIC32_DMAC);
    object_initialize_child(obj, "wdt", &s->wdt, TYPE_PIC32_WDT);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_PIC32_GPIO);
    for (i = 0; i < PIC32_NUM_UARTS; i++) {
        g_autofree char *name = g_strdup_printf("uart%u", i + 1);

        object_initialize_child(obj, name, &s->uart[i], TYPE_PIC32_UART);
    }
    for (i = 0; i < PIC32_NUM_SPIS; i++) {
        g_autofree char *name = g_strdup_printf("spi%u", pic32_spi_number(i));

        object_initialize_child(obj, name, &s->spi[i], TYPE_PIC32_SPI);
    }
    for (i = 0; i < PIC32_NUM_TIMERS; i++) {
        g_autofree char *name = g_strdup_printf("timer%u", i + 1);

        object_initialize_child(obj, name, &s->timer[i], TYPE_PIC32_TIMER);
    }
}

static void pic32mk_soc_realize(DeviceState *dev, Error **errp)
{
    PIC32MKSocState *s = PIC32MK_SOC(dev);
    PIC32MKSocClass *sc = PIC32MK_SOC_GET_CLASS(dev);
    MemoryRegion *system = get_system_memory();
    unsigned i;

    /*
     * The core clock, and the peripheral bus clock the modelled peripherals
     * hang off. Both are constants: the configuration words pick the PLL and
     * this firmware never switches away from it, so the CRU registers only
     * have to agree with what is published here rather than drive it.
     */
    clock_set_hz(s->sysclk, sc->sysclk_hz);
    clock_set_hz(s->pbclk, sc->sysclk_hz / sc->pbclk_div);

    s->cpu = MIPS_CPU(object_new(sc->cpu_type));
    /*
     * CP0 Count runs at half the core clock on this family, and the CPU
     * divides its input by CCRes to get there, so the input is the core clock
     * itself. Without this the count would run at QEMU's default rate and
     * every _CP0_GET_COUNT() delay loop in the firmware would be out by that
     * ratio.
     */
    qdev_connect_clock_in(DEVICE(s->cpu), "clk-in", s->sysclk);
    if (!qdev_realize(DEVICE(s->cpu), NULL, errp)) {
        return;
    }
    /*
     * The core's own interrupt inputs and its Count/Compare timer. Both are
     * the board's to create -- a CPU realized without them takes a null
     * dereference the first time the guest writes CP0 Compare, which is the
     * first thing FreeRTOS does when it starts its scheduler.
     */
    cpu_mips_irq_init_cpu(s->cpu);
    cpu_mips_clock_init(s->cpu);

    memory_region_init_ram(&s->ram, OBJECT(dev), "pic32.ram", sc->ram_size,
                           &error_fatal);
    memory_region_add_subregion(system, PIC32_RAM_BASE, &s->ram);

    memory_region_init_rom(&s->flash, OBJECT(dev), "pic32.flash",
                           sc->flash_size, &error_fatal);
    memory_region_add_subregion(system, PIC32_FLASH_BASE, &s->flash);

    memory_region_init_rom(&s->boot_flash, OBJECT(dev), "pic32.boot-flash",
                           sc->boot_flash_size, &error_fatal);
    memory_region_add_subregion(system, PIC32_BOOT_BASE, &s->boot_flash);

    create_unimplemented_device("pic32.sfr", PIC32_SFR_BASE, PIC32_SFR_SIZE);

    /*
     * The interrupt controller comes first, because every peripheral realized
     * after it wants a line from it.
     */
    object_property_set_link(OBJECT(&s->evic), "cpu", OBJECT(s->cpu),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->evic), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->evic), 0, PIC32_EVIC_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->evic), 1,
                    PIC32_EVIC_BASE + PIC32_EVIC_OFF_BASE);

    /*
     * The core's own three sources reach the controller the same way a
     * peripheral's do. Replacing the CPU's interrupt inputs is the point:
     * left alone they would write the Cause pending bits directly, which in
     * EIC mode are the priority the controller is asking for, not eight
     * separate lines.
     */
    s->cpu->env.irq[0] = pic32_soc_irq_level(s, PIC32_IRQ_CORE_SW0);
    s->cpu->env.irq[1] = pic32_soc_irq_level(s, PIC32_IRQ_CORE_SW1);
    s->cpu->env.irq[(s->cpu->env.CP0_IntCtl >> CP0IntCtl_IPTI) & 0x7] =
        pic32_soc_irq_level(s, PIC32_IRQ_CORE_TIMER);

    qdev_prop_set_uint32(DEVICE(&s->cru), "devid", sc->devid);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->cru), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->cru), 0, PIC32_CFG_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->cru), 1, PIC32_CRU_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pps), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pps), 0, PIC32_PPS_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio), 0, PIC32_GPIO_BASE);

    for (i = 0; i < PIC32_NUM_UARTS; i++) {
        static const hwaddr base[] = { PIC32_UART1_BASE, PIC32_UART2_BASE };
        static const unsigned first[] = { PIC32_IRQ_UART1_FAULT,
                                          PIC32_IRQ_UART2_FAULT };
        unsigned line;

        qdev_prop_set_chr(DEVICE(&s->uart[i]), "chardev", serial_hd(i));
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->uart[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->uart[i]), 0, base[i]);
        for (line = 0; line < PIC32_UART_IRQS; line++) {
            qdev_connect_gpio_out_named(DEVICE(&s->uart[i]),
                                        PIC32_UART_IRQ_GPIO, line,
                                        pic32_soc_irq_level(s, first[i] + line));
        }
    }

    for (i = 0; i < PIC32_NUM_SPIS; i++) {
        static const hwaddr base[] = { PIC32_SPI1_BASE, PIC32_SPI3_BASE,
                                       PIC32_SPI4_BASE };
        static const unsigned first[] = { PIC32_IRQ_SPI1_FAULT,
                                          PIC32_IRQ_SPI3_FAULT,
                                          PIC32_IRQ_SPI4_FAULT };
        unsigned line;

        /*
         * SPI4's data output can be routed to the LED data pin, so it has to
         * be able to time bits rather than hand whole words to a bus.
         */
        if (pic32_spi_number(i) == 4) {
            qdev_prop_set_bit(DEVICE(&s->spi[i]), "serial-out", true);
        }
        qdev_connect_clock_in(DEVICE(&s->spi[i]), "pbclk", s->pbclk);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->spi[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi[i]), 0, base[i]);
        for (line = 0; line < PIC32_SPI_IRQS; line++) {
            qdev_connect_gpio_out_named(DEVICE(&s->spi[i]),
                                        PIC32_SPI_IRQ_GPIO, line,
                                        pic32_soc_irq_level(s,
                                                            first[i] + line));
        }
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dmac), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dmac), 0, PIC32_DMAC_BASE);
    for (i = 0; i < PIC32_DMAC_CHANNELS; i++) {
        /*
         * The channels' sources are not contiguous: 0-3 sit together, 4-7
         * were added later and live up in IFS5.
         */
        unsigned source = i < 4 ? PIC32_IRQ_DMA0 + i : PIC32_IRQ_DMA4 + i - 4;

        qdev_connect_gpio_out_named(DEVICE(&s->dmac), PIC32_DMAC_IRQ_GPIO, i,
                                    qdev_get_gpio_in_named(DEVICE(&s->evic),
                                                           PIC32_EVIC_IRQ_GPIO,
                                                           source));
    }
    /*
     * A channel can be started by any source, so the controller has to see
     * them all. They already converge on the interrupt controller, which
     * echoes them here rather than every peripheral being wired twice.
     */
    s->evic.dmac = &s->dmac;

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->wdt), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->wdt), 0, PIC32_WDT_BASE);
    qdev_connect_gpio_out_named(DEVICE(&s->wdt), PIC32_WDT_TIMEOUT_GPIO, 0,
                                qemu_allocate_irq(pic32mk_soc_wdt_timeout,
                                                  s, 0));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pmp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pmp), 0, PIC32_PMP_BASE);
    qdev_connect_gpio_out_named(DEVICE(&s->pmp), PIC32_PMP_IRQ_GPIO, 0,
                                qdev_get_gpio_in_named(DEVICE(&s->evic),
                                                       PIC32_EVIC_IRQ_GPIO,
                                                       PIC32_IRQ_PMP));
    /*
     * The port in master mode is a metronome: one cycle per word, about four
     * peripheral-bus clocks with zero wait states (~67 ns at 60 MHz). Telling
     * the DMA controller lets it move a port-triggered block in one go and
     * charge the bus time on the virtual clock, instead of paying a
     * main-loop round trip per word -- see "Batched transfers" in
     * pic32_dmac.c.
     */
    pic32_dmac_set_source_pacing(&s->dmac, PIC32_IRQ_PMP, 67);

    for (i = 0; i < PIC32_NUM_TIMERS; i++) {
        static const unsigned source[] = { PIC32_IRQ_TIMER1, PIC32_IRQ_TIMER2,
                                           PIC32_IRQ_TIMER3 };

        qdev_prop_set_bit(DEVICE(&s->timer[i]), "type-a", i == 0);
        qdev_connect_clock_in(DEVICE(&s->timer[i]), "pbclk", s->pbclk);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->timer[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->timer[i]), 0,
                        PIC32_TIMER1_BASE + i * PIC32_TIMER_STRIDE);
        /*
         * A timer's period match is a pulse, not a level: the flag it sets
         * stays set until software clears it, which the data sheet's table
         * records as this source not being persistent.
         */
        qdev_connect_gpio_out_named(DEVICE(&s->timer[i]),
                                    PIC32_TIMER_IRQ_GPIO, 0,
                                    qdev_get_gpio_in_named(DEVICE(&s->evic),
                                                           PIC32_EVIC_IRQ_GPIO,
                                                           source[i]));
    }

    /*
     * Every block a Peripheral Module Disable bit can stop watches one of
     * the flags pic32mk_soc_cfg_changed() maintains; registering the hook
     * last means it never runs before the blocks it speaks for exist.
     */
    for (i = 0; i < PIC32_NUM_TIMERS; i++) {
        pic32_regs_set_pmd_gate(
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->timer[i]), 0),
            &s->pmd_gate[PIC32_PMD_GATE_TIMER1 + i]);
    }
    for (i = 0; i < PIC32_NUM_UARTS; i++) {
        pic32_regs_set_pmd_gate(
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->uart[i]), 0),
            &s->pmd_gate[PIC32_PMD_GATE_UART1 + i]);
    }
    for (i = 0; i < PIC32_NUM_SPIS; i++) {
        pic32_regs_set_pmd_gate(
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->spi[i]), 0),
            &s->pmd_gate[PIC32_PMD_GATE_SPI1 + i]);
    }
    pic32_regs_set_pmd_gate(sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->pmp), 0),
                            &s->pmd_gate[PIC32_PMD_GATE_PMP]);
    pic32_regs_set_pmd_gate(sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->dmac),
                                                   0),
                            &s->pmd_gate[PIC32_PMD_GATE_DMAC]);
    pic32_cru_set_cfg_notify(&s->cru, pic32mk_soc_cfg_changed, s);
}

static void pic32mk_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = pic32mk_soc_realize;
    dc->user_creatable = false;
}

static void pic32mk1024gpk100_soc_class_init(ObjectClass *oc, const void *data)
{
    PIC32MKSocClass *sc = PIC32MK_SOC_CLASS(oc);

    sc->cpu_type = MIPS_CPU_TYPE_NAME("microAptiv-MCU");
    /*
     * FRC 8 MHz through the system PLL as the firmware's configuration words
     * set it up: IDIV 1, MULT 60, ODIV 4. The peripheral bus modules use runs
     * at half that, which the firmware agrees with twice over -- its FreeRTOS
     * configuration says 60 MHz and its U1BRG of 129 gives 115200 baud only at
     * that rate.
     */
    sc->sysclk_hz = 120 * 1000 * 1000;
    sc->pbclk_div = 2;
    sc->ram_size = 256 * KiB;
    sc->flash_size = 1 * MiB;
    sc->boot_flash_size = 20 * KiB;
    /* PIC32MK1024GPK100, with silicon revision B2 in bits 31:28. */
    sc->devid = 0x38B0D053;
}

static const TypeInfo pic32mk_soc_types[] = {
    {
        .name = TYPE_PIC32MK_SOC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC32MKSocState),
        .instance_init = pic32mk_soc_init,
        .class_size = sizeof(PIC32MKSocClass),
        .class_init = pic32mk_soc_class_init,
        .abstract = true,
    },
    {
        .name = TYPE_PIC32MK1024GPK100_SOC,
        .parent = TYPE_PIC32MK_SOC,
        .class_init = pic32mk1024gpk100_soc_class_init,
    },
};

DEFINE_TYPES(pic32mk_soc_types)
