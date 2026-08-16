/*
 * PIC16F15354 and PIC16F15355 microcontrollers, and the XMASNGFMv2 FM Radio
 * board
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties-system.h"
#include "system/system.h"
#include "qom/object.h"
#include "pic16f1_soc.h"
#include "hw/chips/adf4002.h"
#include "boot.h"

struct PIC16F15354MachineState {
    MachineState parent_obj;

    PIC16F1SocState soc;
};
typedef struct PIC16F15354MachineState PIC16F15354MachineState;

/*
 * The '354 and the '355 differ only in how much flash and SRAM they carry, so
 * both are this one machine with the SoC type as class data.
 */
struct PIC16F15354MachineClass {
    MachineClass parent_class;

    const char *soc_type;
};
typedef struct PIC16F15354MachineClass PIC16F15354MachineClass;

#define TYPE_PIC16F15354_MACHINE MACHINE_TYPE_NAME("pic16f15354")
#define TYPE_PIC16F15355_MACHINE MACHINE_TYPE_NAME("pic16f15355")
DECLARE_OBJ_CHECKERS(PIC16F15354MachineState, PIC16F15354MachineClass,
                     PIC16F15354_MACHINE, TYPE_PIC16F15354_MACHINE)

static void pic16f15354_init(MachineState *machine)
{
    PIC16F15354MachineState *m = PIC16F15354_MACHINE(machine);
    PIC16F15354MachineClass *pmc = PIC16F15354_MACHINE_GET_CLASS(machine);

    object_initialize_child(OBJECT(machine), "soc", &m->soc, pmc->soc_type);
    sysbus_realize(SYS_BUS_DEVICE(&m->soc), &error_abort);

    if (machine->firmware) {
        if (!pic16_load_firmware(machine->firmware, &m->soc.flash)) {
            exit(1);
        }
        pic16_load_config_words(&m->soc.cpu);
    }
}

static void pic16f15354_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PIC16F15354MachineClass *pmc = PIC16F15354_MACHINE_CLASS(oc);

    pmc->soc_type = TYPE_PIC16F15354_SOC;
    mc->desc = "Microchip PIC16F15354";
    mc->init = pic16f15354_init;
    mc->default_cpu_type = PIC16_CPU_TYPE_NAME("pic16f1");
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static void pic16f15355_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PIC16F15354MachineClass *pmc = PIC16F15354_MACHINE_CLASS(oc);

    pmc->soc_type = TYPE_PIC16F15355_SOC;
    mc->desc = "Microchip PIC16F15355";
}

struct XmasNgFmMachineState {
    MachineState parent_obj;

    PIC16F1SocState soc;
    ADF4002State adf4002;
};
typedef struct XmasNgFmMachineState XmasNgFmMachineState;

#define TYPE_XMASNGFM_MACHINE MACHINE_TYPE_NAME("xmasngfm")
DECLARE_INSTANCE_CHECKER(XmasNgFmMachineState, XMASNGFM_MACHINE,
                         TYPE_XMASNGFM_MACHINE)

static void xmasngfm_init(MachineState *machine)
{
    XmasNgFmMachineState *m = XMASNGFM_MACHINE(machine);

    object_initialize_child(OBJECT(machine), "soc", &m->soc,
                            TYPE_PIC16F15354_SOC);
    sysbus_realize(SYS_BUS_DEVICE(&m->soc), &error_abort);

    /* ADF4002 PLL synthesizer on MSSP2 SPI master */
    object_initialize_child(OBJECT(machine), "adf4002", &m->adf4002,
                            TYPE_ADF4002);
    qdev_realize(DEVICE(&m->adf4002), BUS(m->soc.mssp2.ssi), &error_abort);

    /*
     * Pin wiring for ADF4002:
     * - MUXOUT (lock detect) -> RA6 (IO_PLL_MUX_OUT)
     * - RC1 (IO_PLL_LE)      -> LE
     * - RC2 (IO_PLL_CE)      -> CE
     */
    qdev_connect_gpio_out_named(DEVICE(&m->adf4002), ADF4002_MUX_OUT_GPIO, 0,
                                qdev_get_gpio_in_named(DEVICE(&m->soc.port),
                                                       PIC16_PORT_IN_GPIO, 6));

    qdev_connect_gpio_out(DEVICE(&m->soc.port), 2 * PIC16_PORT_PINS + 1,
                          qdev_get_gpio_in_named(DEVICE(&m->adf4002),
                                                 ADF4002_LE_GPIO, 0));

    qdev_connect_gpio_out(DEVICE(&m->soc.port), 2 * PIC16_PORT_PINS + 2,
                          qdev_get_gpio_in_named(DEVICE(&m->adf4002),
                                                 ADF4002_CE_GPIO, 0));

    if (machine->firmware) {
        if (!pic16_load_firmware(machine->firmware, &m->soc.flash)) {
            exit(1);
        }
        pic16_load_config_words(&m->soc.cpu);
    }
}

static void xmasngfm_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "XMASNGFMv2 FM Radio Board (PIC16F15354 + ADF4002 PLL)";
    mc->init = xmasngfm_init;
    mc->default_cpu_type = PIC16_CPU_TYPE_NAME("pic16f1");
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static const TypeInfo pic16f15354_machine_types[] = {
    {
        .name = TYPE_PIC16F15354_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(PIC16F15354MachineState),
        .class_size = sizeof(PIC16F15354MachineClass),
        .class_init = pic16f15354_machine_class_init,
    },
    {
        .name = TYPE_PIC16F15355_MACHINE,
        .parent = TYPE_PIC16F15354_MACHINE,
        .class_init = pic16f15355_machine_class_init,
    },
    {
        .name = TYPE_XMASNGFM_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(XmasNgFmMachineState),
        .class_init = xmasngfm_machine_class_init,
    },
};

DEFINE_TYPES(pic16f15354_machine_types)
