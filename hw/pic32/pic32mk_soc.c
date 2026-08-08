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
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "qapi/error.h"
#include "system/system.h"
#include "pic32mk_soc.h"

/* A held line into the interrupt controller, for a source that drives one. */
static qemu_irq pic32_soc_irq_level(PIC32MKSocState *s, unsigned source)
{
    return qdev_get_gpio_in_named(DEVICE(&s->evic),
                                  PIC32_EVIC_IRQ_LEVEL_GPIO, source);
}

/* Which controller each entry of the spi[] array is. */
static const unsigned pic32_spi_numbers[PIC32_NUM_SPIS] = { 1, 3 };

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
                                          PIC32_IRQ_UART1_FAULT + 3 };
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
        static const hwaddr base[] = { PIC32_SPI1_BASE, PIC32_SPI3_BASE };
        static const unsigned first[] = { PIC32_IRQ_SPI1_FAULT,
                                          PIC32_IRQ_SPI3_FAULT };
        unsigned line;

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
    sc->devid = 0;
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
