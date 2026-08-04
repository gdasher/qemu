/*
 * PIC16F17546 board
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "pic16f1_soc.h"
#include "boot.h"

struct PIC16F17546MachineState {
    MachineState parent_obj;

    PIC16F1SocState soc;
};
typedef struct PIC16F17546MachineState PIC16F17546MachineState;

#define TYPE_PIC16F17546_MACHINE MACHINE_TYPE_NAME("pic16f17546")
DECLARE_INSTANCE_CHECKER(PIC16F17546MachineState, PIC16F17546_MACHINE,
                         TYPE_PIC16F17546_MACHINE)

static void pic16f17546_init(MachineState *machine)
{
    PIC16F17546MachineState *m = PIC16F17546_MACHINE(machine);

    object_initialize_child(OBJECT(machine), "soc", &m->soc,
                            TYPE_PIC16F17546_SOC);
    sysbus_realize(SYS_BUS_DEVICE(&m->soc), &error_abort);

    if (machine->firmware) {
        if (!pic16_load_firmware(machine->firmware, &m->soc.flash)) {
            exit(1);
        }
    }
}

static void pic16f17546_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Microchip PIC16F17546";
    mc->init = pic16f17546_init;
    mc->default_cpu_type = PIC16_CPU_TYPE_NAME("pic16f1");
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static const TypeInfo pic16f17546_machine_types[] = {
    {
        .name = TYPE_PIC16F17546_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(PIC16F17546MachineState),
        .class_init = pic16f17546_machine_class_init,
    },
};

DEFINE_TYPES(pic16f17546_machine_types)
