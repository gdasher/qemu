/*
 * Sandcastle polar sand plotter
 *
 * The chips on the board: a PIC16F17546 controller and an MCP23S08 sensor
 * expander on the SPI link, wired as the schematic has them. Everything the
 * board is *for* -- what the pins mean, how the mechanism responds -- lives
 * outside QEMU, behind the simulation bridge.
 *
 * The only product knowledge here is chip-to-chip wiring: the expander's chip
 * select is on RC7 and its interrupt on RA2. That comes off a schematic and
 * describes hardware. Which pin is a step and which is a limit switch does not
 * appear, and must not.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/core/split-irq.h"
#include "qom/object.h"
#include "system/system.h"
#include "pic16f1_soc.h"
#include "pic16_sim_bridge.h"
#include "mcp23s08.h"
#include "boot.h"

/* Expander wiring, from the schematic. */
#define PIN_RA2 2       /* expander INT  */
#define PIN_RC7 23      /* expander CS#  */

struct SandcastleMachineState {
    MachineState parent_obj;

    PIC16F1SocState soc;
    PIC16SimBridge bridge;
    MCP23S08State *expander;
};
typedef struct SandcastleMachineState SandcastleMachineState;

#define TYPE_SANDCASTLE_MACHINE MACHINE_TYPE_NAME("sandcastle")
DECLARE_INSTANCE_CHECKER(SandcastleMachineState, SANDCASTLE_MACHINE,
                         TYPE_SANDCASTLE_MACHINE)

/*
 * Package pins of the 20-pin part, by their data sheet names and the port
 * model's line numbering. RB0 to RB3 are not bonded out on this package.
 */
static const struct {
    const char *name;
    int line;
} sandcastle_soc_pins[] = {
    { "soc.RA0",  0 }, { "soc.RA1",  1 }, { "soc.RA2",  2 },
    { "soc.RA3",  3 }, { "soc.RA4",  4 }, { "soc.RA5",  5 },
    { "soc.RB4", 12 }, { "soc.RB5", 13 }, { "soc.RB6", 14 },
    { "soc.RB7", 15 },
    { "soc.RC0", 16 }, { "soc.RC1", 17 }, { "soc.RC2", 18 },
    { "soc.RC3", 19 }, { "soc.RC4", 20 }, { "soc.RC5", 21 },
    { "soc.RC6", 22 }, { "soc.RC7", 23 },
};

static void sandcastle_init(MachineState *machine)
{
    SandcastleMachineState *m = SANDCASTLE_MACHINE(machine);
    DeviceState *port, *bridge, *expander;
    unsigned i;

    object_initialize_child(OBJECT(machine), "soc", &m->soc,
                            TYPE_PIC16F17546_SOC);
    sysbus_realize(SYS_BUS_DEVICE(&m->soc), &error_abort);
    port = DEVICE(&m->soc.port);

    /* The sensor expander hangs off MSSP1 with CS# on RC7. */
    expander = ssi_create_peripheral(m->soc.mssp1.ssi, TYPE_MCP23S08);
    m->expander = MCP23S08(expander);
    qdev_connect_gpio_out_named(expander, MCP23S08_INT_GPIO, 0,
                                qdev_get_gpio_in_named(port,
                                                       PIC16_PORT_IN_GPIO,
                                                       PIN_RA2));

    /*
     * Without a second serial there is no external model, and the board is
     * just the two chips with nothing attached to their pins -- which is a
     * perfectly good way to run firmware that does not need the mechanism.
     */
    if (serial_hd(1)) {
        object_initialize_child(OBJECT(machine), "bridge", &m->bridge,
                                TYPE_PIC16_SIM_BRIDGE);
        bridge = DEVICE(&m->bridge);
        qdev_prop_set_chr(bridge, "chardev", serial_hd(1));

        for (i = 0; i < ARRAY_SIZE(sandcastle_soc_pins); i++) {
            pic16_sim_bridge_add_line(&m->bridge,
                                      sandcastle_soc_pins[i].name);
        }
        for (i = 0; i < MCP23S08_PINS; i++) {
            g_autofree char *name = g_strdup_printf("expander.GP%u", i);

            pic16_sim_bridge_add_line(&m->bridge, name);
        }

        sysbus_realize(SYS_BUS_DEVICE(&m->bridge), &error_abort);

        /*
         * Each SoC line runs both ways: the chip's output reaches the bridge,
         * and the bridge can drive the chip's input. Which direction is live
         * at any moment is TRIS's business, inside the port model.
         */
        for (i = 0; i < ARRAY_SIZE(sandcastle_soc_pins); i++) {
            int line = sandcastle_soc_pins[i].line;
            qemu_irq sink = qdev_get_gpio_in(bridge, i);

            /*
             * RC7 already drives the expander's chip select, so it has to fan
             * out rather than be reconnected: qdev_connect_gpio_out() replaces
             * any previous connection, and quietly leaving the expander
             * without a chip select makes every SPI read return zero.
             */
            if (line == PIN_RC7) {
                DeviceState *split = qdev_new(TYPE_SPLIT_IRQ);

                qdev_prop_set_uint32(split, "num-lines", 2);
                qdev_realize_and_unref(split, NULL, &error_abort);
                qdev_connect_gpio_out(split, 0,
                                      qdev_get_gpio_in_named(expander,
                                                             SSI_GPIO_CS, 0));
                qdev_connect_gpio_out(split, 1, sink);
                sink = qdev_get_gpio_in(split, 0);
            }
            qdev_connect_gpio_out(port, line, sink);
            qdev_connect_gpio_out(bridge, i,
                                  qdev_get_gpio_in_named(port,
                                                         PIC16_PORT_IN_GPIO,
                                                         line));
        }
        for (i = 0; i < MCP23S08_PINS; i++) {
            qdev_connect_gpio_out(bridge,
                                  ARRAY_SIZE(sandcastle_soc_pins) + i,
                                  qdev_get_gpio_in_named(expander,
                                                         MCP23S08_IN_GPIO, i));
        }
    }

    if (machine->firmware) {
        if (!pic16_load_firmware(machine->firmware, &m->soc.flash)) {
            exit(1);
        }
        pic16_load_config_words(&m->soc.cpu);
    }
}

static void sandcastle_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Sandcastle polar sand plotter";
    mc->init = sandcastle_init;
    mc->default_cpu_type = PIC16_CPU_TYPE_NAME("pic16f1");
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static const TypeInfo sandcastle_types[] = {
    {
        .name = TYPE_SANDCASTLE_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(SandcastleMachineState),
        .class_init = sandcastle_machine_class_init,
    },
};

DEFINE_TYPES(sandcastle_types)
