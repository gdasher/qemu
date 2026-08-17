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
#include "hw/chips/led_demux.h"
#include "hw/pic32/pic32_pps.h"
#include "hw/pic32/pic32_spi.h"
#include "hw/chips/parallel_sram.h"
#include "hw/chips/ws2812.h"
#include "hw/chips/fm_transmitter.h"
#include "hw/chips/fm_link.h"
#include "chardev/char-fe.h"
#include "hw/display/ws2812_panel.h"
#include "hw/sd/sd.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "boot.h"
#include "pic32mk_soc.h"

/*
 * Separators for the list properties: '/' between chips, ':' within one, and
 * '+' between the pins of a group. A comma would read better than the last of
 * those and cannot be used: the machine's own options are comma-separated.
 */
#define SPEC_SEP "/"
#define FIELD_SEP ":"
#define SELECT_SEP "+"

#define MAX_CHIPS 8

typedef struct {
    unsigned spi;       /* which controller, by its number */
    int cs;             /* the port line that selects it */
    unsigned addr;      /* hardware address, for a chip that has one */
    int aux;            /* interrupt line, or -1 */
    uint8_t straps;     /* what its input pins are wired to */
} ChipSpec;

struct PIC32DevboardState {
    MachineState parent_obj;

    PIC32MKSocState soc;

    char *sdcard;
    char *expanders;
    char *sram;
    char *leds;
    char *led_dump;
    char *audio_dump;
    char *fm;
    char *fm_link;
    int64_t fm_clock_ppm;
    char *led_order;
    char *logic_trace;
    char *relays;
    bool watchdog;

    ChipSpec sd;
    bool have_sd;
    ChipSpec fm_spec;
    bool have_fm;
    unsigned relay_addr;        /* the expander whose outputs are relays */
    unsigned n_relays;          /* how many of its pins are wired to one */
    ChipSpec expander[MAX_CHIPS];
    unsigned n_expanders;

    ParallelSramState pmp_sram;
    LedDemuxState demux;
    WS2812PanelState panel;

    /*
     * A port line can have more than one consumer -- two expanders share a
     * chip select on this board -- and qdev_connect_gpio_out() replaces rather
     * than adds, so every extra consumer goes through a fan-out.
     */
    qemu_irq extra[PIC32_GPIO_LINES];

    /*
     * What is driving the LED data pin. The port latch drives it while the
     * firmware bit-bangs; peripheral pin select can hand it to SPI4 instead,
     * and then the strings are fed by a DMA channel rather than by the CPU.
     * Both sources stay wired and this says which one the pin listens to.
     */
    struct {
        qemu_irq sink;
        unsigned rpnr;          /* the output select that governs the pin */
        bool from_spi;
        bool gpio_level;
        bool spi_level;
    } led_src;

    VcdTrace *trace;
    int sig_src;                /* which driver the data pin is listening to */
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

/* Parses "spi<n>:<pin>[:<address>[:<pin>[:<straps>]]]" into one chip's wiring. */
static bool pic32_devboard_parse_chip(const char *spec, const char *kind,
                                      bool has_addr, ChipSpec *out,
                                      Error **errp)
{
    g_auto(GStrv) fields = g_strsplit(spec, FIELD_SEP, 5);
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
    out->straps = 0;
    if (n > 4 && *fields[4]) {
        if (!has_addr) {
            error_setg(errp, "%s: '%s' has no input pins to strap", kind,
                       spec);
            return false;
        }
        if (qemu_strtoul(fields[4], NULL, 0, &value) < 0 || value > 0xff) {
            error_setg(errp, "%s: '%s' is not a byte for the input pins",
                       kind, fields[4]);
            return false;
        }
        out->straps = value;
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
static void pic32_devboard_led_update(PIC32DevboardState *m)
{
    bool level = m->led_src.from_spi ? m->led_src.spi_level
                                     : m->led_src.gpio_level;

    if (m->trace) {
        vcd_trace_set(m->trace, m->sig_src, m->led_src.from_spi);
    }
    qemu_set_irq(m->led_src.sink, level);
}

static void pic32_devboard_led_gpio(void *opaque, int line, int level)
{
    PIC32DevboardState *m = opaque;

    m->led_src.gpio_level = level;
    pic32_devboard_led_update(m);
}

static void pic32_devboard_led_spi(void *opaque, int line, int level)
{
    PIC32DevboardState *m = opaque;

    m->led_src.spi_level = level;
    pic32_devboard_led_update(m);
}

/*
 * A relay moved. They are not pixels and do not latch a frame, so the panel
 * shows them as blocks of their own rather than as part of a string.
 */
static void pic32_devboard_relay(void *opaque, int line, int level)
{
    PIC32DevboardState *m = opaque;

    ws2812_panel_set_switch(&m->panel, line, level);
}

/* Peripheral pin select changed. Only the LED data pin is watched. */
static void pic32_devboard_pps_out(void *opaque, unsigned reg, unsigned sel)
{
    PIC32DevboardState *m = opaque;

    if (reg != m->led_src.rpnr) {
        return;
    }
    m->led_src.from_spi = sel == PIC32_PPS_OUT_SDO4;
    pic32_devboard_led_update(m);
}

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

/*
 * The transmitter, as a link to the microcontroller that really is one. The
 * chip select and the bus are the same as for the model; everything past the
 * pins is a second QEMU running the transmitter's own firmware, and this end
 * only carries bytes and the time they were clocked at. See hw/chips/fm_link.c.
 */
static void pic32_devboard_fit_fm_link(PIC32DevboardState *m,
                                       const ChipSpec *spec)
{
    DeviceState *chip = qdev_new(TYPE_FM_LINK);
    Chardev *chr = qemu_chr_find(m->fm_link);

    if (!chr) {
        error_report("fm-link: there is no chardev called '%s'", m->fm_link);
        exit(1);
    }
    qdev_prop_set_chr(chip, "chardev", chr);
    if (m->audio_dump) {
        qdev_prop_set_string(chip, "dump", m->audio_dump);
    }
    qdev_prop_set_uint8(chip, "cs", 0x7f);
    ssi_realize_and_unref(chip, m->soc.spi[spec->spi].ssi, &error_fatal);

    pic32_devboard_drive(m, spec->cs,
                         qdev_get_gpio_in_named(chip, SSI_GPIO_CS, 0));
}

static void pic32_devboard_fit_fm(PIC32DevboardState *m, const ChipSpec *spec)
{
    DeviceState *chip;

    if (m->fm_link && *m->fm_link) {
        pic32_devboard_fit_fm_link(m, spec);
        return;
    }

    chip = qdev_new(TYPE_FM_TRANSMITTER);
    if (m->audio_dump) {
        qdev_prop_set_string(chip, "dump", m->audio_dump);
    }
    if (MACHINE(m)->audiodev) {
        qdev_prop_set_string(chip, "audiodev", MACHINE(m)->audiodev);
    }
    qdev_prop_set_int32(chip, "clock-ppm", (int32_t)m->fm_clock_ppm);
    /*
     * The transmitter shares SPI3 with the port expanders, whose select
     * indexes are their hardware addresses (0 and 1); the index only has to
     * be distinct, since a port pin does the selecting here too.
     */
    qdev_prop_set_uint8(chip, "cs", 0x7f);
    ssi_realize_and_unref(chip, m->soc.spi[spec->spi].ssi, &error_fatal);

    pic32_devboard_drive(m, spec->cs,
                         qdev_get_gpio_in_named(chip, SSI_GPIO_CS, 0));
}

/*
 * The relays, as "<address>[:<count>]": which expander drives them, and how
 * many of its eight pins reach one. An expander is already described by the
 * expanders option; this only says what the board hung off the one it names.
 */
static bool pic32_devboard_parse_relays(PIC32DevboardState *m, Error **errp)
{
    g_auto(GStrv) fields = g_strsplit(m->relays, FIELD_SEP, 2);
    unsigned n = g_strv_length(fields);
    unsigned long value;

    if (qemu_strtoul(fields[0], NULL, 0, &value) < 0 || value > 3) {
        error_setg(errp, "relays: '%s' is not an expander address between 0 "
                   "and 3", fields[0]);
        return false;
    }
    m->relay_addr = value;

    m->n_relays = MCP23S08_PINS;
    if (n > 1 && *fields[1]) {
        if (qemu_strtoul(fields[1], NULL, 0, &value) < 0 ||
            !value || value > MCP23S08_PINS) {
            error_setg(errp, "relays: '%s' is not a count between 1 and %d",
                       fields[1], MCP23S08_PINS);
            return false;
        }
        m->n_relays = value;
    }
    return true;
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
    /*
     * Whatever the board wires the input pins to. On this one they are a
     * switch bank the firmware reads its device ID off, and a device ID
     * decides which of a movie's regions belong to this board -- so a board
     * left unstrapped plays only the movies written for device zero.
     */
    for (unsigned bit = 0; bit < 8; bit++) {
        qemu_set_irq(qdev_get_gpio_in_named(chip, MCP23S08_IN_GPIO, bit),
                     (spec->straps >> bit) & 1);
    }
    if (spec->aux >= 0) {
        qdev_connect_gpio_out_named(chip, MCP23S08_INT_GPIO, 0,
                                    qdev_get_gpio_in_named(
                                        DEVICE(&m->soc.gpio),
                                        PIC32_GPIO_IN_GPIO, spec->aux));
    }
    if (m->n_relays && spec->addr == m->relay_addr) {
        unsigned i;

        for (i = 0; i < m->n_relays; i++) {
            qdev_connect_gpio_out_named(chip, MCP23S08_OUT_GPIO, i,
                                        qemu_allocate_irq(
                                            pic32_devboard_relay, m, i));
        }
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

/*
 * The LED strings, as "<data>:<count>x<pixels>[:<select>,...[:<enable>]]".
 * They are bit-banged from one pin through a demultiplexer that picks which
 * string is listening, which is how a part with one spare output drives eight:
 *
 *   leds=RA14:8x600:RA1+RB0+RB1:RA11
 *
 * The strings are watched by a panel, which shows them as one row of pixels
 * each and writes every frame to the file named by led-dump.
 */
static bool pic32_devboard_fit_leds(PIC32DevboardState *m, Error **errp)
{
    g_auto(GStrv) fields = g_strsplit(m->leds, FIELD_SEP, 4);
    unsigned n = g_strv_length(fields);
    unsigned long strings, pixels;
    const char *rest;
    int din, enable = -1;
    unsigned i;

    if (n < 2) {
        error_setg(errp, "leds: '%s' needs a data pin and a size", m->leds);
        return false;
    }
    din = pic32_devboard_pin(fields[0], errp);
    if (din < 0) {
        return false;
    }
    if (qemu_strtoul(fields[1], &rest, 10, &strings) < 0 || *rest != 'x' ||
        qemu_strtoul(rest + 1, NULL, 10, &pixels) < 0 ||
        !strings || strings > LED_DEMUX_MAX_OUTPUTS || !pixels) {
        error_setg(errp, "leds: '%s' is not <strings>x<pixels>", fields[1]);
        return false;
    }
    if (n > 3 && *fields[3]) {
        enable = pic32_devboard_pin(fields[3], errp);
        if (enable < 0) {
            return false;
        }
    }

    object_initialize_child(OBJECT(m), "led-demux", &m->demux,
                            TYPE_LED_DEMUX);
    qdev_prop_set_uint32(DEVICE(&m->demux), "outputs", strings);
    /*
     * A board that wires no enable line has nothing to switch the part on, so
     * it starts on. One that does starts off, as a pulled-up input would.
     */
    qdev_prop_set_bit(DEVICE(&m->demux), "disabled", enable >= 0);
    if (!sysbus_realize(SYS_BUS_DEVICE(&m->demux), errp)) {
        return false;
    }

    /*
     * The data pin has two possible drivers. The port latch is one; SPI4's
     * serial output is the other, once the firmware points the pin's output
     * select at it. Both are wired, and peripheral pin select says which one
     * the demultiplexer hears. RPAnR is one register per bit of port A; the
     * other ports are not laid out to the same formula and no board here
     * needs them.
     */
    m->led_src.sink = qdev_get_gpio_in_named(DEVICE(&m->demux),
                                             LED_DEMUX_IN_GPIO, 0);
    if (din / PIC32_GPIO_PINS == 0) {
        m->led_src.rpnr = PIC32_PPS_OUT(0x1600 + (din % PIC32_GPIO_PINS) * 4);
        pic32_pps_set_out_notifier(&m->soc.pps, pic32_devboard_pps_out, m);
    } else {
        m->led_src.rpnr = PIC32_PPS_REGS;       /* never matches */
        warn_report("leds: the data pin is not on port A, so SPI cannot be "
                    "routed to it");
    }

    pic32_devboard_drive(m, din,
                         qemu_allocate_irq(pic32_devboard_led_gpio, m, 0));
    for (i = 0; i < PIC32_NUM_SPIS; i++) {
        if (pic32_spi_number(i) == 4) {
            qdev_connect_gpio_out_named(DEVICE(&m->soc.spi[i]),
                                        PIC32_SPI_SDO_GPIO, 0,
                                        qemu_allocate_irq(
                                            pic32_devboard_led_spi, m, 0));
        }
    }
    if (enable >= 0) {
        pic32_devboard_drive(m, enable,
                             qdev_get_gpio_in_named(DEVICE(&m->demux),
                                                    LED_DEMUX_ENABLE_GPIO, 0));
    }
    if (n > 2 && *fields[2]) {
        g_auto(GStrv) pins = g_strsplit(fields[2], SELECT_SEP, -1);

        for (i = 0; pins[i]; i++) {
            int line = pic32_devboard_pin(pins[i], errp);

            if (line < 0) {
                return false;
            }
            pic32_devboard_drive(m, line,
                                 qdev_get_gpio_in_named(DEVICE(&m->demux),
                                                        LED_DEMUX_SELECT_GPIO,
                                                        i));
        }
    }

    object_initialize_child(OBJECT(m), "led-panel", &m->panel,
                            TYPE_WS2812_PANEL);
    qdev_prop_set_uint32(DEVICE(&m->panel), "strips", strings);
    qdev_prop_set_uint32(DEVICE(&m->panel), "pixels", pixels);
    qdev_prop_set_uint32(DEVICE(&m->panel), "switches", m->n_relays);
    if (m->led_dump) {
        qdev_prop_set_string(DEVICE(&m->panel), "dump", m->led_dump);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&m->panel), errp)) {
        return false;
    }

    for (i = 0; i < strings; i++) {
        DeviceState *strip = qdev_new(TYPE_WS2812);
        g_autofree char *name = g_strdup_printf("led%u", i);

        qdev_prop_set_uint32(strip, "pixels", pixels);
        qdev_prop_set_string(strip, "name", name);
        if (m->led_order) {
            qdev_prop_set_string(strip, "order", m->led_order);
        }
        object_property_add_child(OBJECT(m), name, OBJECT(strip));
        ws2812_panel_add(&m->panel, WS2812(strip));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(strip), &error_fatal);

        qdev_connect_gpio_out_named(DEVICE(&m->demux), LED_DEMUX_OUT_GPIO, i,
                                    qdev_get_gpio_in_named(strip,
                                                           WS2812_IN_GPIO, 0));
    }
    return true;
}

/*
 * The logic trace, as "<file>[:<start ms>[:<length ms>]]". It watches the LED
 * lines, which are the fastest thing on the board and the reason the window
 * exists: a bit-banged string changes its data line twice a microsecond for
 * as long as the movie lasts, so a whole run is gigabytes of waveform. The
 * default window is a quarter of a second from the start of the run, which is
 * a frame or two of most movies.
 */
static bool pic32_devboard_fit_trace(PIC32DevboardState *m, Error **errp)
{
    g_auto(GStrv) fields = g_strsplit(m->logic_trace, FIELD_SEP, 3);
    unsigned n = g_strv_length(fields);
    uint64_t start_ms = 0, len_ms = 250;

    if (!*fields[0]) {
        error_setg(errp, "logic-trace: '%s' names no file", m->logic_trace);
        return false;
    }
    if (n > 1 && qemu_strtou64(fields[1], NULL, 10, &start_ms) < 0) {
        error_setg(errp, "logic-trace: '%s' is not a start in ms", fields[1]);
        return false;
    }
    if (n > 2 && qemu_strtou64(fields[2], NULL, 10, &len_ms) < 0) {
        error_setg(errp, "logic-trace: '%s' is not a length in ms", fields[2]);
        return false;
    }

    m->trace = vcd_trace_new(fields[0], start_ms * SCALE_MS, len_ms * SCALE_MS,
                             errp);
    if (!m->trace) {
        return false;
    }
    led_demux_set_trace(&m->demux, m->trace);
    m->sig_src = vcd_trace_add(m->trace, "spi_drives_din", 1);
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
    if (m->fm && *m->fm) {
        if (!pic32_devboard_parse_chip(m->fm, "fm", false, &m->fm_spec,
                                       &error_fatal)) {
            exit(1);
        }
        m->have_fm = true;
    } else if ((m->audio_dump && *m->audio_dump) ||
               (m->fm_link && *m->fm_link)) {
        if (!pic32_devboard_parse_chip("spi3:RD15", "fm", false, &m->fm_spec,
                                       &error_fatal)) {
            exit(1);
        }
        m->have_fm = true;
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

    if (m->relays && *m->relays &&
        !pic32_devboard_parse_relays(m, &error_fatal)) {
        exit(1);
    }
    if (m->sram && *m->sram && !pic32_devboard_fit_sram(m, &error_fatal)) {
        exit(1);
    }
    if (m->leds && *m->leds && !pic32_devboard_fit_leds(m, &error_fatal)) {
        exit(1);
    }
    if (m->logic_trace && *m->logic_trace) {
        if (!m->leds || !*m->leds) {
            error_report("logic-trace: there are no LED lines to trace");
            exit(1);
        }
        if (!pic32_devboard_fit_trace(m, &error_fatal)) {
            exit(1);
        }
    }
    if (m->have_sd) {
        pic32_devboard_fit_sd(m, &m->sd);
    }
    if (m->have_fm) {
        pic32_devboard_fit_fm(m, &m->fm_spec);
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

static char *pic32_devboard_get_leds(Object *obj, Error **errp)
{
    return g_strdup(PIC32_DEVBOARD_MACHINE(obj)->leds);
}

static void pic32_devboard_set_leds(Object *obj, const char *value,
                                    Error **errp)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(obj);

    g_free(m->leds);
    m->leds = g_strdup(value);
}

static char *pic32_devboard_get_led_dump(Object *obj, Error **errp)
{
    return g_strdup(PIC32_DEVBOARD_MACHINE(obj)->led_dump);
}

static void pic32_devboard_set_led_dump(Object *obj, const char *value,
                                        Error **errp)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(obj);

    g_free(m->led_dump);
    m->led_dump = g_strdup(value);
}

static char *pic32_devboard_get_audio_dump(Object *obj, Error **errp)
{
    return g_strdup(PIC32_DEVBOARD_MACHINE(obj)->audio_dump);
}

static void pic32_devboard_set_audio_dump(Object *obj, const char *value,
                                          Error **errp)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(obj);

    g_free(m->audio_dump);
    m->audio_dump = g_strdup(value);
}

static char *pic32_devboard_get_fm(Object *obj, Error **errp)
{
    return g_strdup(PIC32_DEVBOARD_MACHINE(obj)->fm);
}

static void pic32_devboard_set_fm(Object *obj, const char *value,
                                  Error **errp)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(obj);

    g_free(m->fm);
    m->fm = g_strdup(value);
}

static char *pic32_devboard_get_fm_link(Object *obj, Error **errp)
{
    return g_strdup(PIC32_DEVBOARD_MACHINE(obj)->fm_link);
}

static void pic32_devboard_set_fm_link(Object *obj, const char *value,
                                       Error **errp)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(obj);

    g_free(m->fm_link);
    m->fm_link = g_strdup(value);
}

static char *pic32_devboard_get_led_order(Object *obj, Error **errp)
{
    return g_strdup(PIC32_DEVBOARD_MACHINE(obj)->led_order);
}

static void pic32_devboard_set_led_order(Object *obj, const char *value,
                                         Error **errp)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(obj);

    g_free(m->led_order);
    m->led_order = g_strdup(value);
}

static char *pic32_devboard_get_relays(Object *obj, Error **errp)
{
    return g_strdup(PIC32_DEVBOARD_MACHINE(obj)->relays);
}

static void pic32_devboard_set_relays(Object *obj, const char *value,
                                      Error **errp)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(obj);

    g_free(m->relays);
    m->relays = g_strdup(value);
}

static char *pic32_devboard_get_logic_trace(Object *obj, Error **errp)
{
    return g_strdup(PIC32_DEVBOARD_MACHINE(obj)->logic_trace);
}

static void pic32_devboard_set_logic_trace(Object *obj, const char *value,
                                           Error **errp)
{
    PIC32DevboardState *m = PIC32_DEVBOARD_MACHINE(obj);

    g_free(m->logic_trace);
    m->logic_trace = g_strdup(value);
}

static void pic32_devboard_get_fm_clock_ppm(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    int64_t value = PIC32_DEVBOARD_MACHINE(obj)->fm_clock_ppm;

    visit_type_int(v, name, &value, errp);
}

static void pic32_devboard_set_fm_clock_ppm(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < -100000 || value > 100000) {
        error_setg(errp, "fm-clock-ppm must be within +/-100000 (10%%)");
        return;
    }
    PIC32_DEVBOARD_MACHINE(obj)->fm_clock_ppm = value;
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

    machine_add_audiodev_property(mc);

    object_class_property_add_str(oc, "sdcard", pic32_devboard_get_sdcard,
                                  pic32_devboard_set_sdcard);
    object_class_property_set_description(oc, "sdcard",
        "SD card as controller:chip-select, e.g. spi1:RD8");

    object_class_property_add_str(oc, "sram", pic32_devboard_get_sram,
                                  pic32_devboard_set_sram);
    object_class_property_set_description(oc, "sram",
        "static RAM on the parallel port as words[:chip-select-line], "
        "e.g. 1048576:0x800000");

    object_class_property_add_str(oc, "leds", pic32_devboard_get_leds,
                                  pic32_devboard_set_leds);
    object_class_property_set_description(oc, "leds",
        "WS2812 strings as data-pin:count x pixels[:select-pins[:enable-pin]],"
        " e.g. RA14:8x600:RA1+RB0+RB1:RA11");

    object_class_property_add_str(oc, "led-dump",
                                  pic32_devboard_get_led_dump,
                                  pic32_devboard_set_led_dump);
    object_class_property_set_description(oc, "led-dump",
        "write every latched LED frame to this file");

    object_class_property_add_str(oc, "audio-dump",
                                  pic32_devboard_get_audio_dump,
                                  pic32_devboard_set_audio_dump);
    object_class_property_set_description(oc, "audio-dump",
        "write every received FM audio packet and config command to this file");

    object_class_property_add_str(oc, "fm",
                                  pic32_devboard_get_fm,
                                  pic32_devboard_set_fm);
    object_class_property_set_description(oc, "fm",
        "FM transmitter as controller:chip-select, e.g. spi3:RD15");

    object_class_property_add_str(oc, "fm-link",
                                  pic32_devboard_get_fm_link,
                                  pic32_devboard_set_fm_link);
    object_class_property_set_description(oc, "fm-link",
        "run the FM transmitter's own firmware in a second QEMU instead of "
        "modelling it: the chardev reaching that machine, e.g. a socket. The "
        "chip select and the bus are as the fm option describes them; "
        "audio-dump then records the bytes exchanged rather than a model's "
        "reading of them");

    object_class_property_add(oc, "fm-clock-ppm", "int",
                              pic32_devboard_get_fm_clock_ppm,
                              pic32_devboard_set_fm_clock_ppm, NULL, NULL);
    object_class_property_set_description(oc, "fm-clock-ppm",
        "the FM transmitter's oscillator error in parts per million "
        "(positive: its sample clock runs fast), e.g. -10000 for 1% slow");

    object_class_property_add_str(oc, "relays", pic32_devboard_get_relays,
                                  pic32_devboard_set_relays);
    object_class_property_set_description(oc, "relays",
        "on/off outputs on an expander, as address[:count], e.g. 1:3");

    object_class_property_add_str(oc, "logic-trace",
                                  pic32_devboard_get_logic_trace,
                                  pic32_devboard_set_logic_trace);
    object_class_property_set_description(oc, "logic-trace",
        "write a VCD of the LED lines to this file, as "
        "file[:start-ms[:length-ms]], e.g. leds.vcd:2000:250");

    object_class_property_add_str(oc, "led-order",
                                  pic32_devboard_get_led_order,
                                  pic32_devboard_set_led_order);
    object_class_property_set_description(oc, "led-order",
        "byte order the strings expect, 'grb' as the data sheet has it or "
        "'rgb' for the parts that take red first");

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
        "MCP23S08 expanders as "
        "controller:chip-select[:address[:interrupt[:input-pins]]] "
        "separated by '/', e.g. spi3:RA4:0::1/spi3:RA4:1");
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
