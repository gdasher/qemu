/*
 * PIC18 instruction-test machine
 *
 * A bare harness for exercising the instruction set: program flash and the
 * other program-space regions the HEX file can carry, a flat data RAM, and a
 * small test device that lets a fixture report results, raise the two
 * interrupt request lines and terminate. It is not a model of any real part
 * -- the SoC for that arrives with the peripherals.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "target/pic18/cpu.h"
#include "qom/object.h"

/*
 * The test device sits in bank 1 of the SFR space, at addresses the Q43
 * firmware never touches (0x100-0x107 hold no register on the real part
 * that this firmware uses).
 */
#define PIC18_TESTDEV_BASE 0x0100
#define PIC18_TESTDEV_SIZE 8

#define TESTDEV_PUTCHAR  0
#define TESTDEV_EXIT     1
#define TESTDEV_DUMP     2
#define TESTDEV_IRQ_HIGH 3
#define TESTDEV_IRQ_LOW  4

struct PIC18TestMachineState {
    MachineState parent_obj;

    PIC18CPU *cpu;
    qemu_irq irq[2];
    MemoryRegion flash;
    MemoryRegion userid;
    MemoryRegion config;
    MemoryRegion eeprom;
    MemoryRegion data;
    MemoryRegion testdev;
};
typedef struct PIC18TestMachineState PIC18TestMachineState;

#define TYPE_PIC18_TEST_MACHINE MACHINE_TYPE_NAME("pic18-test")
DECLARE_INSTANCE_CHECKER(PIC18TestMachineState, PIC18_TEST_MACHINE,
                         TYPE_PIC18_TEST_MACHINE)

static uint64_t pic18_testdev_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void pic18_testdev_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    PIC18TestMachineState *m = opaque;
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
    case TESTDEV_IRQ_HIGH:
    case TESTDEV_IRQ_LOW:
        /* Stands in for the SoC asserting a request line. */
        qemu_set_irq(m->irq[addr - TESTDEV_IRQ_HIGH], value & 1);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pic18_testdev_ops = {
    .read = pic18_testdev_read,
    .write = pic18_testdev_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static bool pic18_test_load_firmware(const char *filename, MemoryRegion *flash)
{
    g_autofree char *path = NULL;
    ssize_t bytes_loaded;
    uint64_t entry = 0;

    path = qemu_find_file(QEMU_FILE_TYPE_BIOS, filename);
    if (path == NULL) {
        error_report("unable to find firmware image '%s'", filename);
        return false;
    }

    /*
     * Intel HEX carries the program, the configuration bytes and the user
     * IDs at their program-space addresses, which is how the regions are laid
     * out here, so it loads in one pass. Fall back to a raw flash image.
     */
    bytes_loaded = load_targphys_hex_as(path, &entry, &address_space_memory);
    if (bytes_loaded >= 0) {
        return true;
    }

    bytes_loaded = load_image_mr(path, flash);
    if (bytes_loaded < 0) {
        error_report("unable to load '%s' as Intel HEX or raw binary",
                     filename);
        return false;
    }
    return true;
}

/*
 * The HEX loader registers ROM blobs that are copied in at the first reset,
 * so the configuration bytes have to be read from the blob itself at board
 * init; the CPU wants them (STVREN) before the guest runs an instruction.
 */
static void pic18_test_load_config(PIC18CPU *cpu)
{
    unsigned i;

    for (i = 0; i < PIC18_NUM_CONFIG; i++) {
        hwaddr addr = OFFSET_CODE + PIC18_CONFIG_BASE + i;
        uint8_t byte;
        const uint8_t *blob = rom_ptr_for_as(&address_space_memory, addr, 1);

        if (blob) {
            byte = *blob;
        } else {
            address_space_read(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, &byte, 1);
        }
        cpu->env.config[i] = byte;
    }
}

static void pic18_test_init(MachineState *machine)
{
    PIC18TestMachineState *m = PIC18_TEST_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();

    m->cpu = PIC18_CPU(cpu_create(machine->cpu_type));

    memory_region_init_rom(&m->flash, NULL, "pic18.flash",
                           PIC18_FLASH_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, OFFSET_CODE, &m->flash);

    memory_region_init_rom(&m->userid, NULL, "pic18.userid", 0x400,
                           &error_fatal);
    memory_region_add_subregion(system_memory,
                                OFFSET_CODE + PIC18_USERID_BASE, &m->userid);

    memory_region_init_rom(&m->config, NULL, "pic18.config", 0x400,
                           &error_fatal);
    memory_region_add_subregion(system_memory,
                                OFFSET_CODE + PIC18_CONFIG_BASE, &m->config);

    memory_region_init_rom(&m->eeprom, NULL, "pic18.eeprom",
                           PIC18_EEPROM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory,
                                OFFSET_CODE + PIC18_EEPROM_BASE, &m->eeprom);

    /*
     * Flat data memory covering the whole 14-bit space, SFR banks included.
     * The real part's device map belongs to the SoC; fixtures for the
     * instruction set do not depend on it.
     */
    memory_region_init_ram(&m->data, NULL, "pic18.data",
                           PIC18_DATA_SPACE_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, OFFSET_DATA, &m->data);

    m->irq[0] = qdev_get_gpio_in(DEVICE(m->cpu), PIC18_IRQ_HIGH);
    m->irq[1] = qdev_get_gpio_in(DEVICE(m->cpu), PIC18_IRQ_LOW);

    memory_region_init_io(&m->testdev, OBJECT(machine), &pic18_testdev_ops,
                          m, "pic18.testdev", PIC18_TESTDEV_SIZE);
    memory_region_add_subregion_overlap(system_memory,
                                        OFFSET_DATA + PIC18_TESTDEV_BASE,
                                        &m->testdev, 1);

    if (machine->firmware) {
        if (!pic18_test_load_firmware(machine->firmware, &m->flash)) {
            exit(1);
        }
        pic18_test_load_config(m->cpu);
    }
}

static void pic18_test_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "PIC18 instruction-test harness";
    mc->init = pic18_test_init;
    mc->default_cpu_type = PIC18_CPU_TYPE_NAME("pic18q43");
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static const TypeInfo pic18_test_machine_types[] = {
    {
        .name = TYPE_PIC18_TEST_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(PIC18TestMachineState),
        .class_init = pic18_test_machine_class_init,
    },
};

DEFINE_TYPES(pic18_test_machine_types)
