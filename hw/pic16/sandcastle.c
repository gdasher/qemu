/*
 * Sandcastle polar sand plotter
 *
 * The PIC16F17546 controller plus the parts of the machine the firmware can
 * observe: the MCP23S08 sensor expander on the isoSPI link, and enough of the
 * mechanics to close the loop between step pulses and the switches that
 * homing waits for.
 *
 * The mechanics model is deliberately kinematic, not physical: it counts step
 * pulses and asserts a switch when the count reaches the position that switch
 * sits at. That is all the firmware can see, and it is what makes G28
 * completable and step output checkable.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "pic16f1_soc.h"
#include "mcp23s08.h"
#include "boot.h"

/*
 * Pin numbering matches the port device: port * 8 + pin. From the board
 * schematic, via the firmware's pins.h.
 */
#define PIN_RA2 2       /* CTRL2_INT,  in  */
#define PIN_RA4 4       /* THETA_DIR,  out */
#define PIN_RA5 5       /* THETA_CLK,  out */
#define PIN_RB5 13      /* LIMIT1,     in  */
#define PIN_RC3 19      /* RADIUS_DIR, out */
#define PIN_RC4 20      /* RADIUS_CLK, out */
#define PIN_RC7 23      /* CTRL2_CS#,  out */

/* Expander pins, from ctrl2.h. */
#define GP_THETA_INDEX 0
#define GP_LIMIT_OUTER 1

/* Mechanics, derived from the firmware's config.h. */
#define THETA_STEPS_PER_REV 1200        /* 200 * 6:1 gearing */
#define THETA_INDEX_WIDTH   60          /* arc the tape covers, in steps */
#define RADIUS_STEPS_PER_MM 250         /* 200 * 2 microsteps / 1.6 mm */
#define RADIUS_START_STEPS  100         /* just off the inner switch */

#define TYPE_SANDCASTLE_RIG "sandcastle-rig"
OBJECT_DECLARE_SIMPLE_TYPE(SandcastleRigState, SANDCASTLE_RIG)

enum {
    RIG_IN_THETA_DIR,
    RIG_IN_THETA_CLK,
    RIG_IN_RADIUS_DIR,
    RIG_IN_RADIUS_CLK,
    RIG_IN_LINES,
};

struct SandcastleRigState {
    /*
     * A SysBusDevice rather than a bare DeviceState purely so that it lands on
     * the main system bus and therefore in the reset tree: an unparented
     * device never gets its reset handler called, and would silently start
     * from zeroed state -- which here means sitting on both limit switches.
     */
    SysBusDevice parent_obj;

    int32_t theta;      /* steps, wraps at a machine revolution */
    int32_t radius;     /* steps from the inner switch */
    bool theta_dir;
    bool radius_dir;
    bool theta_clk;
    bool radius_clk;

    qemu_irq limit1;
    qemu_irq theta_index;
    qemu_irq limit_outer;
};

static void sandcastle_rig_update(SandcastleRigState *s)
{
    int32_t theta = s->theta % THETA_STEPS_PER_REV;

    if (theta < 0) {
        theta += THETA_STEPS_PER_REV;
    }

    /*
     * LIMIT1 has a pulldown on the board, so a closed switch reads high. The
     * switch is also the mechanical hard stop, so the carriage cannot go past.
     */
    qemu_set_irq(s->limit1, s->radius <= 0);
    qemu_set_irq(s->theta_index, theta < THETA_INDEX_WIDTH);
    qemu_set_irq(s->limit_outer, 0);
}

static void sandcastle_rig_set_line(void *opaque, int line, int level)
{
    SandcastleRigState *s = SANDCASTLE_RIG(opaque);

    switch (line) {
    case RIG_IN_THETA_DIR:
        s->theta_dir = level;
        return;
    case RIG_IN_RADIUS_DIR:
        s->radius_dir = level;
        return;
    case RIG_IN_THETA_CLK:
        if (level && !s->theta_clk) {
            s->theta += s->theta_dir ? 1 : -1;
        }
        s->theta_clk = level;
        break;
    case RIG_IN_RADIUS_CLK:
        if (level && !s->radius_clk) {
            s->radius += s->radius_dir ? 1 : -1;
            if (s->radius < 0) {
                s->radius = 0;  /* the switch is the hard stop */
            }
        }
        s->radius_clk = level;
        break;
    default:
        return;
    }
    sandcastle_rig_update(s);
}

static void sandcastle_rig_reset_hold(Object *obj, ResetType type)
{
    SandcastleRigState *s = SANDCASTLE_RIG(obj);

    /*
     * Start just off the inner switch and a third of a turn away from the
     * index tape, so homing has to seek for both rather than starting on them.
     */
    s->radius = RADIUS_START_STEPS;
    s->theta = THETA_STEPS_PER_REV / 2;
    s->theta_dir = false;
    s->radius_dir = false;
    s->theta_clk = false;
    s->radius_clk = false;
    sandcastle_rig_update(s);
}

static void sandcastle_rig_realize(DeviceState *dev, Error **errp)
{
    SandcastleRigState *s = SANDCASTLE_RIG(dev);

    qdev_init_gpio_in(dev, sandcastle_rig_set_line, RIG_IN_LINES);
    qdev_init_gpio_out_named(dev, &s->limit1, "limit1", 1);
    qdev_init_gpio_out_named(dev, &s->theta_index, "theta-index", 1);
    qdev_init_gpio_out_named(dev, &s->limit_outer, "limit-outer", 1);
}

static const VMStateDescription sandcastle_rig_vmstate = {
    .name = "sandcastle-rig",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(theta, SandcastleRigState),
        VMSTATE_INT32(radius, SandcastleRigState),
        VMSTATE_BOOL(theta_dir, SandcastleRigState),
        VMSTATE_BOOL(radius_dir, SandcastleRigState),
        VMSTATE_BOOL(theta_clk, SandcastleRigState),
        VMSTATE_BOOL(radius_clk, SandcastleRigState),
        VMSTATE_END_OF_LIST()
    }
};

static void sandcastle_rig_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = sandcastle_rig_realize;
    dc->vmsd = &sandcastle_rig_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = sandcastle_rig_reset_hold;
}

/*
 * Board
 */

struct SandcastleMachineState {
    MachineState parent_obj;

    PIC16F1SocState soc;
    SandcastleRigState rig;
    MCP23S08State *expander;
};
typedef struct SandcastleMachineState SandcastleMachineState;

#define TYPE_SANDCASTLE_MACHINE MACHINE_TYPE_NAME("sandcastle")
DECLARE_INSTANCE_CHECKER(SandcastleMachineState, SANDCASTLE_MACHINE,
                         TYPE_SANDCASTLE_MACHINE)

static void sandcastle_init(MachineState *machine)
{
    SandcastleMachineState *m = SANDCASTLE_MACHINE(machine);
    DeviceState *port, *rig, *expander;

    object_initialize_child(OBJECT(machine), "soc", &m->soc,
                            TYPE_PIC16F17546_SOC);
    sysbus_realize(SYS_BUS_DEVICE(&m->soc), &error_abort);
    port = DEVICE(&m->soc.port);

    object_initialize_child(OBJECT(machine), "rig", &m->rig,
                            TYPE_SANDCASTLE_RIG);
    sysbus_realize(SYS_BUS_DEVICE(&m->rig), &error_abort);
    rig = DEVICE(&m->rig);

    /* The sensor expander hangs off MSSP1 with CS# on RC7. */
    expander = ssi_create_peripheral(m->soc.mssp1.ssi, TYPE_MCP23S08);
    m->expander = MCP23S08(expander);
    qdev_connect_gpio_out(port, PIN_RC7,
                          qdev_get_gpio_in_named(expander, SSI_GPIO_CS, 0));

    /* Its interrupt line is the arm board's INT, wired to RA2. */
    qdev_connect_gpio_out_named(expander, MCP23S08_INT_GPIO, 0,
                                qdev_get_gpio_in_named(port,
                                                       PIC16_PORT_IN_GPIO,
                                                       PIN_RA2));

    /* Step and direction outputs feed the mechanics. */
    qdev_connect_gpio_out(port, PIN_RA4, qdev_get_gpio_in(rig,
                                                          RIG_IN_THETA_DIR));
    qdev_connect_gpio_out(port, PIN_RA5, qdev_get_gpio_in(rig,
                                                          RIG_IN_THETA_CLK));
    qdev_connect_gpio_out(port, PIN_RC3, qdev_get_gpio_in(rig,
                                                          RIG_IN_RADIUS_DIR));
    qdev_connect_gpio_out(port, PIN_RC4, qdev_get_gpio_in(rig,
                                                          RIG_IN_RADIUS_CLK));

    /* And the switches it drives feed back to the controller. */
    qdev_connect_gpio_out_named(rig, "limit1", 0,
                                qdev_get_gpio_in_named(port,
                                                       PIC16_PORT_IN_GPIO,
                                                       PIN_RB5));
    qdev_connect_gpio_out_named(rig, "theta-index", 0,
                                qdev_get_gpio_in_named(expander,
                                                       MCP23S08_IN_GPIO,
                                                       GP_THETA_INDEX));
    qdev_connect_gpio_out_named(rig, "limit-outer", 0,
                                qdev_get_gpio_in_named(expander,
                                                       MCP23S08_IN_GPIO,
                                                       GP_LIMIT_OUTER));

    if (machine->firmware) {
        if (!pic16_load_firmware(machine->firmware, &m->soc.flash)) {
            exit(1);
        }
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
        .name = TYPE_SANDCASTLE_RIG,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SandcastleRigState),
        .class_init = sandcastle_rig_class_init,
    },
    {
        .name = TYPE_SANDCASTLE_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(SandcastleMachineState),
        .class_init = sandcastle_machine_class_init,
    },
};

DEFINE_TYPES(sandcastle_types)
