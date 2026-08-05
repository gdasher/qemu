/*
 * PIC16 development board
 *
 * A PIC16F17546, an MCP23S08 I/O expander on its SPI port, and a simulation
 * bridge carrying every package pin. It is not a model of any particular
 * product: the expander's wiring is a machine property, and what the pins mean
 * is entirely the business of whatever connects to the bridge.
 *
 * That makes it the board to point a new firmware at. Everything the guest can
 * touch is either a modelled chip or a line on the bridge.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/split-irq.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "system/system.h"
#include "pic16f1_soc.h"
#include "pic16_sim_bridge.h"
#include "mcp23s08.h"
#include "boot.h"

#define DEFAULT_EXPANDER_CS "RC7"
#define DEFAULT_EXPANDER_INT "RA2"

struct PIC16DevboardState {
    MachineState parent_obj;

    PIC16F1SocState soc;
    PIC16SimBridge bridge;
    MCP23S08State *expander;

    char *expander_cs;
    char *expander_int;
};
typedef struct PIC16DevboardState PIC16DevboardState;

#define TYPE_PIC16_DEVBOARD_MACHINE MACHINE_TYPE_NAME("pic16-devboard")
DECLARE_INSTANCE_CHECKER(PIC16DevboardState, PIC16_DEVBOARD_MACHINE,
                         TYPE_PIC16_DEVBOARD_MACHINE)

/*
 * Package pins of the 20-pin part, by their data sheet names and the port
 * model's line numbering. RB0 to RB3 are not bonded out on this package.
 */
static const struct {
    const char *name;
    int line;
} devboard_pins[] = {
    { "RA0",  0 }, { "RA1",  1 }, { "RA2",  2 },
    { "RA3",  3 }, { "RA4",  4 }, { "RA5",  5 },
    { "RB4", 12 }, { "RB5", 13 }, { "RB6", 14 }, { "RB7", 15 },
    { "RC0", 16 }, { "RC1", 17 }, { "RC2", 18 }, { "RC3", 19 },
    { "RC4", 20 }, { "RC5", 21 }, { "RC6", 22 }, { "RC7", 23 },
};

static int devboard_pin(const char *name, Error **errp)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(devboard_pins); i++) {
        if (!strcmp(devboard_pins[i].name, name)) {
            return devboard_pins[i].line;
        }
    }
    error_setg(errp, "'%s' is not a pin on this package", name);
    return -1;
}

static void devboard_init(MachineState *machine)
{
    PIC16DevboardState *m = PIC16_DEVBOARD_MACHINE(machine);
    DeviceState *port, *bridge, *expander;
    int cs_line, int_line;
    unsigned i;

    cs_line = devboard_pin(m->expander_cs ?: DEFAULT_EXPANDER_CS,
                           &error_fatal);
    int_line = devboard_pin(m->expander_int ?: DEFAULT_EXPANDER_INT,
                            &error_fatal);

    object_initialize_child(OBJECT(machine), "soc", &m->soc,
                            TYPE_PIC16F17546_SOC);
    sysbus_realize(SYS_BUS_DEVICE(&m->soc), &error_abort);
    port = DEVICE(&m->soc.port);

    expander = ssi_create_peripheral(m->soc.mssp1.ssi, TYPE_MCP23S08);
    m->expander = MCP23S08(expander);
    qdev_connect_gpio_out_named(expander, MCP23S08_INT_GPIO, 0,
                                qdev_get_gpio_in_named(port,
                                                       PIC16_PORT_IN_GPIO,
                                                       int_line));

    /*
     * Without a second serial there is no external model, and the board is
     * just the two chips with nothing attached to their pins -- which is a
     * perfectly good way to run firmware that does not need one.
     */
    if (serial_hd(1)) {
        object_initialize_child(OBJECT(machine), "bridge", &m->bridge,
                                TYPE_PIC16_SIM_BRIDGE);
        bridge = DEVICE(&m->bridge);
        qdev_prop_set_chr(bridge, "chardev", serial_hd(1));

        for (i = 0; i < ARRAY_SIZE(devboard_pins); i++) {
            g_autofree char *name =
                g_strdup_printf("soc.%s", devboard_pins[i].name);

            pic16_sim_bridge_add_line(&m->bridge, name);
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
        for (i = 0; i < ARRAY_SIZE(devboard_pins); i++) {
            int line = devboard_pins[i].line;
            qemu_irq sink = qdev_get_gpio_in(bridge, i);

            /*
             * The chip select already has a consumer, so it has to fan out
             * rather than be reconnected: qdev_connect_gpio_out() replaces any
             * previous connection, and quietly leaving the expander without a
             * chip select makes every SPI read return zero.
             */
            if (line == cs_line) {
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
            qdev_connect_gpio_out(bridge, ARRAY_SIZE(devboard_pins) + i,
                                  qdev_get_gpio_in_named(expander,
                                                         MCP23S08_IN_GPIO, i));
        }
    } else {
        qdev_connect_gpio_out(port, cs_line,
                              qdev_get_gpio_in_named(expander,
                                                     SSI_GPIO_CS, 0));
    }

    if (machine->firmware) {
        if (!pic16_load_firmware(machine->firmware, &m->soc.flash)) {
            exit(1);
        }
        pic16_load_config_words(&m->soc.cpu);
    }
}

static char *devboard_get_cs(Object *obj, Error **errp)
{
    return g_strdup(PIC16_DEVBOARD_MACHINE(obj)->expander_cs
                    ?: DEFAULT_EXPANDER_CS);
}

static void devboard_set_cs(Object *obj, const char *value, Error **errp)
{
    PIC16DevboardState *m = PIC16_DEVBOARD_MACHINE(obj);

    if (devboard_pin(value, errp) < 0) {
        return;
    }
    g_free(m->expander_cs);
    m->expander_cs = g_strdup(value);
}

static char *devboard_get_int(Object *obj, Error **errp)
{
    return g_strdup(PIC16_DEVBOARD_MACHINE(obj)->expander_int
                    ?: DEFAULT_EXPANDER_INT);
}

static void devboard_set_int(Object *obj, const char *value, Error **errp)
{
    PIC16DevboardState *m = PIC16_DEVBOARD_MACHINE(obj);

    if (devboard_pin(value, errp) < 0) {
        return;
    }
    g_free(m->expander_int);
    m->expander_int = g_strdup(value);
}

static void devboard_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "PIC16F17546 with an SPI I/O expander and simulation bridge";
    mc->init = devboard_init;
    mc->default_cpu_type = PIC16_CPU_TYPE_NAME("pic16f1");
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;

    object_class_property_add_str(oc, "expander-cs",
                                  devboard_get_cs, devboard_set_cs);
    object_class_property_set_description(oc, "expander-cs",
        "package pin driving the expander's chip select (default "
        DEFAULT_EXPANDER_CS ")");
    object_class_property_add_str(oc, "expander-int",
                                  devboard_get_int, devboard_set_int);
    object_class_property_set_description(oc, "expander-int",
        "package pin the expander's interrupt output reaches (default "
        DEFAULT_EXPANDER_INT ")");
}

static const TypeInfo devboard_types[] = {
    {
        .name = TYPE_PIC16_DEVBOARD_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(PIC16DevboardState),
        .class_init = devboard_machine_class_init,
    },
};

DEFINE_TYPES(devboard_types)
