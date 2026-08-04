/*
 * PIC16 instruction-test machine
 *
 * A bare harness for exercising the instruction set: program flash, a flat
 * data RAM, and a small test device that lets a fixture report results and
 * terminate. It is not a model of any real part -- the SoC for that arrives
 * with the peripherals.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "target/pic16/cpu.h"
#include "qom/object.h"
#include "boot.h"

/*
 * The test device occupies the SFR slots of bank 63, which no real part in
 * this family uses for anything a fixture would want.
 */
#define PIC16_TESTDEV_BASE (63 * PIC16_BANK_SIZE + PIC16_CORE_REGS)
#define PIC16_TESTDEV_SIZE 4

#define TESTDEV_PUTCHAR 0
#define TESTDEV_EXIT    1
#define TESTDEV_DUMP    2
#define TESTDEV_IRQ     3

struct PIC16TestMachineState {
    MachineState parent_obj;

    PIC16CPU *cpu;
    qemu_irq irq;
    MemoryRegion flash;
    MemoryRegion config;
    MemoryRegion data;
    MemoryRegion testdev;
};
typedef struct PIC16TestMachineState PIC16TestMachineState;

#define TYPE_PIC16_TEST_MACHINE MACHINE_TYPE_NAME("pic16-test")
DECLARE_INSTANCE_CHECKER(PIC16TestMachineState, PIC16_TEST_MACHINE,
                         TYPE_PIC16_TEST_MACHINE)

static uint64_t pic16_testdev_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void pic16_testdev_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    PIC16TestMachineState *m = opaque;
    CPUState *cs = CPU(m->cpu);

    switch (addr) {
    case TESTDEV_PUTCHAR:
        putchar(value & 0xFF);
        fflush(stdout);
        break;
    case TESTDEV_EXIT:
        fflush(stdout);
        exit(value & 0xFF);
        break;
    case TESTDEV_DUMP:
        cpu_dump_state(cs, stderr, 0);
        break;
    case TESTDEV_IRQ:
        /* Stands in for a peripheral raising an interrupt request. */
        qemu_set_irq(m->irq, value & 1);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pic16_testdev_ops = {
    .read = pic16_testdev_read,
    .write = pic16_testdev_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic16_test_init(MachineState *machine)
{
    PIC16TestMachineState *m = PIC16_TEST_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();

    m->cpu = PIC16_CPU(cpu_create(machine->cpu_type));

    /* Program flash: 32K words, each a 14-bit instruction in a 16-bit word. */
    memory_region_init_rom(&m->flash, NULL, "pic16.flash",
                           PIC16_CODE_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, OFFSET_CODE, &m->flash);

    /* Configuration words, DIA and DCI, at word 0x8000 and up. */
    memory_region_init_rom(&m->config, NULL, "pic16.config",
                           PIC16_CONFIG_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, OFFSET_CONFIG, &m->config);

    /*
     * Flat data memory covering the whole banked space. The real part aliases
     * common RAM into every bank and mirrors the GPR blocks into a linear
     * window; that structure belongs to the SoC, and fixtures for the
     * instruction set do not depend on it.
     */
    memory_region_init_ram(&m->data, NULL, "pic16.data",
                           PIC16_BANKED_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, OFFSET_DATA, &m->data);

    m->irq = qdev_get_gpio_in(DEVICE(m->cpu), 0);

    memory_region_init_io(&m->testdev, OBJECT(machine), &pic16_testdev_ops,
                          m, "pic16.testdev", PIC16_TESTDEV_SIZE);
    memory_region_add_subregion_overlap(system_memory,
                                        OFFSET_DATA + PIC16_TESTDEV_BASE,
                                        &m->testdev, 1);

    if (machine->firmware) {
        if (!pic16_load_firmware(machine->firmware, &m->flash)) {
            exit(1);
        }
    }
}

static void pic16_test_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "PIC16 instruction-test harness";
    mc->init = pic16_test_init;
    mc->default_cpu_type = PIC16_CPU_TYPE_NAME("pic16f1");
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static const TypeInfo pic16_test_machine_types[] = {
    {
        .name = TYPE_PIC16_TEST_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(PIC16TestMachineState),
        .class_init = pic16_test_machine_class_init,
    },
};

DEFINE_TYPES(pic16_test_machine_types)
