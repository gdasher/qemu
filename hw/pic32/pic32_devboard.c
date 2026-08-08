/*
 * PIC32MK development board
 *
 * A PIC32MK1024GPK100 and whatever chips you say are wired to it. It is not a
 * model of any particular product: nothing is fitted by default, the board is
 * described on the command line, and what each pin means is the business of
 * whatever is connected to it.
 *
 *   -M pic32mk-devboard,sdcard=spi1:RD8,expanders=spi3:RA4:0/spi3:RA4:1,\
 *      sram=1048576:0x800000 \
 *      -drive file=sd.img,if=sd,format=raw \
 *      -bios firmware.elf -icount shift=3
 *
 * fits an SD card on SPI1 selected by RD8, two MCP23S08 expanders that share
 * RA4 as their select and answer to hardware addresses 0 and 1, and a
 * million-word static RAM on the parallel port selected by address line 23.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/split-irq.h"
#include "hw/core/sysbus.h"
#include "hw/chips/mcp23s08.h"
#include "hw/chips/parallel_sram.h"
#include "hw/sd/sd.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "boot.h"
#include "pic32mk_soc.h"

/* Separators for the list properties: '/' between chips, ':' within one. */
#define SPEC_SEP "/"
#define FIELD_SEP ":"

#define MAX_CHIPS 8

typedef struct {
    unsigned spi;       /* which controller, by its number */
    int cs;             /* the port line that selects it */
    unsigned addr;      /* hardware address, for a chip that has one */
    int aux;            /* interrupt line, or -1 */
} ChipSpec;

struct PIC32DevboardState {
    MachineState parent_obj;

    PIC32MKSocState soc;

    char *sdcard;
    char *expanders;
    char *sram;
    bool watchdog;

    ChipSpec sd;
    bool have_sd;
    ChipSpec expander[MAX_CHIPS];
    unsigned n_expanders;

    ParallelSramState pmp_sram;

    /*
     * A port line can have more than one consumer -- two expanders share a
     * chip select on this board -- and qdev_connect_gpio_out() replaces rather
     * than adds, so every extra consumer goes through a fan-out.
     */
    qemu_irq extra[PIC32_GPIO_LINES];
};
typedef struct PIC32DevboardState PIC32DevboardState;

#define TYPE_PIC32_DEVBOARD_MACHINE MACHINE_TYPE_NAME("pic32mk-devboard")
DECLARE_INSTANCE_CHECKER(PIC32DevboardState, PIC32_DEVBOARD_MACHINE,
                         TYPE_PIC32_DEVBOARD_MACHINE)

/*
 * Package pins are named for their port and bit -- RD8 is port D bit 8 -- and
 * the port model numbers its lines the same way.
 */
static int pic32_devboard_pin(const char *name, Error **errp)
{
    unsigned long pin;
    const char *rest;
    unsigned port;

    if (name[0] != 'R' || name[1] < 'A' || name[1] >= 'A' + PIC32_GPIO_PORTS) {
        error_setg(errp, "'%s' is not a port pin", name);
        return -1;
    }
    port = name[1] - 'A';
    if (qemu_strtoul(name + 2, &rest, 10, &pin) < 0 || *rest ||
        pin >= PIC32_GPIO_PINS) {
        error_setg(errp, "'%s' is not a port pin", name);
        return -1;
    }
    return port * PIC32_GPIO_PINS + pin;
}

static bool pic32_devboard_controller(const char *name, unsigned *out,
                                      Error **errp)
{
    unsigned i;

    for (i = 0; i < PIC32_NUM_SPIS; i++) {
        g_autofree char *want = g_strdup_printf("spi%u", pic32_spi_number(i));

        if (!strcmp(name, want)) {
            *out = i;
            return true;
        }
    }
    error_setg(errp, "'%s' is not an SPI controller this machine models",
               name);
    return false;
}

/* Parses "spi<n>:<pin>[:<address>[:<pin>]]" into one chip's wiring. */
static bool pic32_devboard_parse_chip(const char *spec, const char *kind,
                                      bool has_addr, ChipSpec *out,
                                      Error **errp)
{
    g_auto(GStrv) fields = g_strsplit(spec, FIELD_SEP, 4);
    unsigned n = g_strv_length(fields);
    unsigned long value;

    if (n < 2) {
        error_setg(errp, "%s: '%s' needs a controller and a chip select",
                   kind, spec);
        return false;
    }
    if (!pic32_devboard_controller(fields[0], &out->spi, errp)) {
        return false;
    }
    out->cs = pic32_devboard_pin(fields[1], errp);
    if (out->cs < 0) {
        return false;
    }

    out->addr = 0;
    out->aux = -1;
    if (n > 2 && *fields[2]) {
        if (!has_addr) {
            error_setg(errp, "%s: '%s' has no hardware address", kind, spec);
            return false;
        }
        if (qemu_strtoul(fields[2], NULL, 10, &value) < 0 || value > 3) {
            error_setg(errp, "%s: '%s' is not an address between 0 and 3",
                       kind, fields[2]);
            return false;
        }
        out->addr = value;
    }
    if (n > 3 && *fields[3]) {
        out->aux = pic32_devboard_pin(fields[3], errp);
        if (out->aux < 0) {
            return false;
        }
    }
    return true;
}

static bool pic32_devboard_parse_list(const char *value, const char *kind,
                                      bool has_addr, ChipSpec *out,
                                      unsigned *n_out, Error **errp)
{
    g_auto(GStrv) specs = NULL;
    unsigned n = 0;

    *n_out = 0;
    if (!value || !*value) {
        return true;
    }

    specs = g_strsplit(value, SPEC_SEP, -1);
    for (unsigned i = 0; specs[i]; i++) {
        if (!*specs[i]) {
            error_setg(errp, "%s: empty entry in '%s'", kind, value);
            return false;
        }
        if (n == MAX_CHIPS) {
            error_setg(errp, "%s: at most %d may be fitted", kind, MAX_CHIPS);
            return false;
        }
        if (!pic32_devboard_parse_chip(specs[i], kind, has_addr, &out[n],
                                       errp)) {
            return false;
        }
        n++;
    }

    *n_out = n;
    return true;
}

/*
 * Hands out another consumer for a port line. The first goes straight on the
 * output; anything after that inserts a splitter, because a silently replaced
 * connection is invisible until some chip stops answering.
 */
static void pic32_devboard_drive(PIC32DevboardState *m, int line,
                                 qemu_irq sink)
{
    DeviceState *gpio = DEVICE(&m->soc.gpio);
    DeviceState *split;

    if (!m->extra[line]) {
        qdev_connect_gpio_out_named(gpio, PIC32_GPIO_OUT_GPIO, line, sink);
        m->extra[line] = sink;
        return;
    }

    split = qdev_new(TYPE_SPLIT_IRQ);
    qdev_prop_set_uint32(split, "num-lines", 2);
    qdev_realize_and_unref(split, NULL, &error_abort);
    qdev_connect_gpio_out(split, 0, m->extra[line]);
    qdev_connect_gpio_out(split, 1, sink);
    qdev_connect_gpio_out_named(gpio, PIC32_GPIO_OUT_GPIO, line,
                                qdev_get_gpio_in(split, 0));
    m->extra[line] = qdev_get_gpio_in(split, 0);
}

/*
 * An SD card in SPI mode, on the bus and selected by a port pin. The firmware
 * drives that pin itself -- Harmony's driver is given the pin number and the
 * controller's own select is disabled -- so the card takes its select from the
 * port rather than from the SPI controller.
 */
static void pic32_devboard_fit_sd(PIC32DevboardState *m, const ChipSpec *spec)
{
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    DeviceState *ssi_sd, *card;

    ssi_sd = ssi_create_peripheral(m->soc.spi[spec->spi].ssi, "ssi-sd");

    card = qdev_new(TYPE_SD_CARD_SPI);
    qdev_prop_set_drive_err(card, "drive",
                            di ? blk_by_legacy_dinfo(di) : NULL, &error_fatal);
    qdev_realize_and_unref(card, qdev_get_child_bus(ssi_sd, "sd-bus"),
                           &error_fatal);

    pic32_devboard_drive(m, spec->cs,
                         qdev_get_gpio_in_named(ssi_sd, SSI_GPIO_CS, 0));
}

static void pic32_devboard_fit_expander(PIC32DevboardState *m,
                                        const ChipSpec *spec)
{
    DeviceState *chip = qdev_new(TYPE_MCP23S08);

    qdev_prop_set_uint8(chip, "address", spec->addr);
    /*
     * The bus insists every peripheral on it has a different select index,
     * because a controller that drives chip select by index has no other way
     * to tell them apart. These are selected by a port pin instead, and two of
     * them share it, so the index is only there to be distinct -- the chip's
     * hardware address serves.
     */
    qdev_prop_set_uint8(chip, "cs", spec->addr);
    ssi_realize_and_unref(chip, m->soc.spi[spec->spi].ssi, &error_fatal);

    pic32_devboard_drive(m, spec->cs,
                         qdev_get_gpio_in_named(chip, SSI_GPIO_CS, 0));
    if (spec->aux >= 0) {
        qdev_connect_gpio_out_named(chip, MCP23S08_INT_GPIO, 0,
                                    qdev_get_gpio_in_named(
                                        DEVICE(&m->soc.gpio),
                                        PIC32_GPIO_IN_GPIO, spec->aux));
    }
}

/*
 * The static RAM on the parallel port, as "<words>[:<select>]" -- how many
 * locations it has, and which address line the board uses as its chip select.
 * The XMASNg board fits a 2 MB part and selects it with A23:
 *
 *   sram=1048576:0x800000
 */
static bool pic32_devboard_fit_sram(PIC32DevboardState *m, Error **errp)
{
    static const PIC32PmpTarget target = {
        .read = parallel_sram_read,
        .write = parallel_sram_write,
    };
    g_auto(GStrv) fields = g_strsplit(m->sram, FIELD_SEP, 2);
    unsigned n = g_strv_length(fields);
    uint64_t words;
    unsigned long select = 0;

    if (n < 1 || qemu_strtou64(fields[0], NULL, 0, &words) < 0 || !words) {
        error_setg(errp, "sram: '%s' does not start with a size in words",
                   m->sram);
        return false;
    }
    if (n > 1 && qemu_strtoul(fields[1], NULL, 0, &select) < 0) {
        error_setg(errp, "sram: '%s' is not an address line", fields[1]);
        return false;
    }

    object_initialize_child(OBJECT(m), "pmp-sram", &m->pmp_sram,
                            TYPE_PARALLEL_SRAM);
    qdev_prop_set_uint64(DEVICE(&m->pmp_sram), "words", words);
    qdev_prop_set_uint32(DEVICE(&m->pmp_sram), "select", select);
    if (!sysbus_realize(SYS_BUS_DEVICE(&m->pmp_sram), errp)) {
        return false;
    }
    pic32_pmp_attach(&m->soc.pmp, &target, &m->pmp_sram);
    return true;
}

static void pic32_devboard_init(MachineState *machine)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(machine);
    const char *firmware = machine->firmware ?: machine->kernel_filename;
    unsigned n, i;

    if (m->sdcard && *m->sdcard) {
        if (!pic32_devboard_parse_chip(m->sdcard, "sdcard", false, &m->sd,
                                       &error_fatal)) {
            exit(1);
        }
        m->have_sd = true;
    }
    if (!pic32_devboard_parse_list(m->expanders, "expanders", true,
                                   m->expander, &n, &error_fatal)) {
        exit(1);
    }
    m->n_expanders = n;

    object_initialize_child(OBJECT(machine), "soc", &m->soc,
                            TYPE_PIC32MK1024GPK100_SOC);
    pic32mk_soc_set_watchdog(&m->soc, m->watchdog);
    sysbus_realize(SYS_BUS_DEVICE(&m->soc), &error_fatal);

    if (m->sram && *m->sram && !pic32_devboard_fit_sram(m, &error_fatal)) {
        exit(1);
    }
    if (m->have_sd) {
        pic32_devboard_fit_sd(m, &m->sd);
    }
    for (i = 0; i < m->n_expanders; i++) {
        pic32_devboard_fit_expander(m, &m->expander[i]);
    }

    if (firmware && !pic32_load_firmware(firmware)) {
        exit(1);
    }
}

static char *pic32_devboard_get_sdcard(Object *obj, Error **errp)
{
    return g_strdup(PIC32_DEVBOARD_MACHINE(obj)->sdcard);
}

static void pic32_devboard_set_sdcard(Object *obj, const char *value,
                                      Error **errp)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(obj);

    g_free(m->sdcard);
    m->sdcard = g_strdup(value);
}

static char *pic32_devboard_get_expanders(Object *obj, Error **errp)
{
    return g_strdup(PIC32_DEVBOARD_MACHINE(obj)->expanders);
}

static void pic32_devboard_set_expanders(Object *obj, const char *value,
                                         Error **errp)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(obj);

    g_free(m->expanders);
    m->expanders = g_strdup(value);
}

static char *pic32_devboard_get_sram(Object *obj, Error **errp)
{
    return g_strdup(PIC32_DEVBOARD_MACHINE(obj)->sram);
}

static void pic32_devboard_set_sram(Object *obj, const char *value,
                                    Error **errp)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(obj);

    g_free(m->sram);
    m->sram = g_strdup(value);
}

static bool pic32_devboard_get_watchdog(Object *obj, Error **errp)
{
    return PIC32_DEVBOARD_MACHINE(obj)->watchdog;
}

static void pic32_devboard_set_watchdog(Object *obj, bool value, Error **errp)
{
    PIC32_DEVBOARD_MACHINE(obj)->watchdog = value;
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

    object_class_property_add_str(oc, "sdcard", pic32_devboard_get_sdcard,
                                  pic32_devboard_set_sdcard);
    object_class_property_set_description(oc, "sdcard",
        "SD card as controller:chip-select, e.g. spi1:RD8");

    object_class_property_add_str(oc, "sram", pic32_devboard_get_sram,
                                  pic32_devboard_set_sram);
    object_class_property_set_description(oc, "sram",
        "static RAM on the parallel port as words[:chip-select-line], "
        "e.g. 1048576:0x800000");

    object_class_property_add_bool(oc, "watchdog",
                                   pic32_devboard_get_watchdog,
                                   pic32_devboard_set_watchdog);
    object_class_property_set_description(oc, "watchdog",
        "arm the watchdog at reset, as the configuration words would. Off by "
        "default: a firmware that stops feeding it is supposed to be reset, "
        "which during bring-up hides whatever stopped it");

    object_class_property_add_str(oc, "expanders",
                                  pic32_devboard_get_expanders,
                                  pic32_devboard_set_expanders);
    object_class_property_set_description(oc, "expanders",
        "MCP23S08 expanders as controller:chip-select[:address[:interrupt]] "
        "separated by '/', e.g. spi3:RA4:0/spi3:RA4:1");
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
