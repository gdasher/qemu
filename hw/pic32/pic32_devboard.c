/*
 * PIC32MK development board
 *
 * A PIC32MK1024GPK100 and whatever chips you say are wired to it. It is not a
 * model of any particular product: nothing is fitted by default, the board is
 * described on the command line, and what each pin means is the business of
 * whatever is connected to it.
 *
 *   -M pic32mk-devboard -bios firmware.elf -icount shift=3 -serial mon:stdio
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "boot.h"
#include "pic32mk_soc.h"

struct PIC32DevboardState {
    MachineState parent_obj;

    PIC32MKSocState soc;
};
typedef struct PIC32DevboardState PIC32DevboardState;

#define TYPE_PIC32_DEVBOARD_MACHINE MACHINE_TYPE_NAME("pic32mk-devboard")
DECLARE_INSTANCE_CHECKER(PIC32DevboardState, PIC32_DEVBOARD_MACHINE,
                         TYPE_PIC32_DEVBOARD_MACHINE)

static void pic32_devboard_init(MachineState *machine)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(machine);
    const char *firmware = machine->firmware ?: machine->kernel_filename;

    object_initialize_child(OBJECT(machine), "soc", &m->soc,
                            TYPE_PIC32MK1024GPK100_SOC);
    sysbus_realize(SYS_BUS_DEVICE(&m->soc), &error_fatal);

    if (firmware && !pic32_load_firmware(firmware)) {
        exit(1);
    }
}

static void pic32_devboard_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Microchip PIC32MK development board";
    mc->init = pic32_devboard_init;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("microAptiv-MCU");
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_id = NULL;
}

static const TypeInfo pic32_devboard_machine_types[] = {
    {
        .name = TYPE_PIC32_DEVBOARD_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(PIC32DevboardState),
        .class_init = pic32_devboard_machine_class_init,
    },
};

DEFINE_TYPES(pic32_devboard_machine_types)
