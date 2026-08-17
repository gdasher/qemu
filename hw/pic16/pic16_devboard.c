/*
 * PIC16 development board
 *
 * A PIC16F17546, a simulation bridge carrying every package pin, and whatever
 * chips you say are wired to it. It is not a model of any particular product
 * and nothing is fitted by default: the board is described on the command
 * line, and what the pins mean is entirely the business of whatever connects
 * to the bridge.
 *
 *   -M pic16-devboard,expanders=RC7:RA2/RC6,leds=RB7:237
 *
 * fits two MCP23S08 expanders -- one selected by RC7 with its interrupt on
 * RA2, one selected by RC6 with its interrupt unconnected -- and a 237-pixel
 * WS2812 strip on RB7. Each chip is named on the bridge after the pin that
 * identifies it, so a model can tell two of a kind apart and the names do not
 * shift when the list is reordered.
 *
 * That makes it the board to point a new firmware at. Everything the guest can
 * touch is either a modelled chip or a line on the bridge.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/split-irq.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "system/system.h"
#include "chardev/char-fe.h"
#include "pic16f1_soc.h"
#include "pic16_sim_bridge.h"
#include "pic16_cosim_link.h"
#include "hw/chips/mcp23s08.h"
#include "hw/chips/ws2812.h"
#include "hw/chips/adf4002.h"
#include "boot.h"

/* Separators for the list properties: '/' between chips, ':' within one. */
#define SPEC_SEP "/"
#define FIELD_SEP ":"

#define MAX_CHIPS 8

typedef struct {
    int pin;            /* chip select, or the strip's data line */
    int aux;            /* interrupt pin, or -1 */
    unsigned count;     /* pixels, for a strip */
    char *name;         /* what the bridge calls it */
} ChipSpec;

struct PIC16DevboardState {
    MachineState parent_obj;

    PIC16F1SocState soc;
    PIC16SimBridge bridge;
    PIC16CosimLink link;

    char *soc_name;
    char *expanders;
    char *leds;
    char *adf4002;
    char *rf_dump;
    char *fm_link;

    /*
     * Where the radio's own outputs go. The bridge is one destination and the
     * rf-dump file is the other; a board can have both, so the sinks below
     * are given the machine rather than either of them.
     */
    PIC16SimBridge *rf_bridge;
    FILE *rf_file;

    ChipSpec expander[MAX_CHIPS];
    unsigned n_expanders;
    ChipSpec led[MAX_CHIPS];
    unsigned n_leds;

    /*
     * A port line can have more than one consumer -- a chip select that the
     * model also watches, say -- and qdev_connect_gpio_out() replaces rather
     * than adds, so every extra consumer goes through a fan-out.
     */
    qemu_irq extra[PIC16_PORT_LINES];
};
typedef struct PIC16DevboardState PIC16DevboardState;

#define TYPE_PIC16_DEVBOARD_MACHINE MACHINE_TYPE_NAME("pic16-devboard")
DECLARE_INSTANCE_CHECKER(PIC16DevboardState, PIC16_DEVBOARD_MACHINE,
                         TYPE_PIC16_DEVBOARD_MACHINE)

/*
 * Package pins of the 20-pin part (PIC16F17546), by their data sheet names and
 * the port model's line numbering. RB0 to RB3 are not bonded out on this package.
 */
static const struct {
    const char *name;
    int line;
} devboard_pins_20pin[] = {
    { "RA0",  0 }, { "RA1",  1 }, { "RA2",  2 },
    { "RA3",  3 }, { "RA4",  4 }, { "RA5",  5 },
    { "RB4", 12 }, { "RB5", 13 }, { "RB6", 14 }, { "RB7", 15 },
    { "RC0", 16 }, { "RC1", 17 }, { "RC2", 18 }, { "RC3", 19 },
    { "RC4", 20 }, { "RC5", 21 }, { "RC6", 22 }, { "RC7", 23 },
};

/*
 * Package pins of the 28-pin parts (PIC16F15354, PIC16F15355).
 */
static const struct {
    const char *name;
    int line;
} devboard_pins_28pin[] = {
    { "RA0",  0 }, { "RA1",  1 }, { "RA2",  2 }, { "RA3",  3 },
    { "RA4",  4 }, { "RA5",  5 }, { "RA6",  6 }, { "RA7",  7 },
    { "RB0",  8 }, { "RB1",  9 }, { "RB2", 10 }, { "RB3", 11 },
    { "RB4", 12 }, { "RB5", 13 }, { "RB6", 14 }, { "RB7", 15 },
    { "RC0", 16 }, { "RC1", 17 }, { "RC2", 18 }, { "RC3", 19 },
    { "RC4", 20 }, { "RC5", 21 }, { "RC6", 22 }, { "RC7", 23 },
    { "RE3", PIC16_PORT_E * PIC16_PORT_PINS + 3 },  /* MCLR/VPP, input only */
};

static int devboard_pin(const char *name, Error **errp)
{
    if (strlen(name) == 3 && name[0] == 'R' &&
        ((name[1] >= 'A' && name[1] <= 'C') || name[1] == 'E') &&
        name[2] >= '0' && name[2] <= '7') {
        int port = name[1] == 'E' ? PIC16_PORT_E : name[1] - 'A';
        int pin = name[2] - '0';
        return port * PIC16_PORT_PINS + pin;
    }
    error_setg(errp, "'%s' is not a valid pin name on this package", name);
    return -1;
}

/*
 * Parses "pin[:extra]/pin[:extra]/..." into specs. The second field is
 * another pin for an expander's interrupt, or a count for a strip's length;
 * which one is the caller's to say, so that a pin where a number belongs is
 * an error rather than a chip that quietly ends up unwired.
 */
static bool devboard_parse(const char *value, const char *kind, bool aux_is_pin,
                           ChipSpec *out, unsigned *n_out, Error **errp)
{
    g_auto(GStrv) specs = NULL;
    unsigned n = 0;

    *n_out = 0;
    if (!value || !*value) {
        return true;
    }

    specs = g_strsplit(value, SPEC_SEP, -1);
    for (unsigned i = 0; specs[i]; i++) {
        g_auto(GStrv) fields = g_strsplit(specs[i], FIELD_SEP, 2);
        ChipSpec *spec;

        if (!*specs[i]) {
            error_setg(errp, "%s: empty entry in '%s'", kind, value);
            return false;
        }
        if (n == MAX_CHIPS) {
            error_setg(errp, "%s: at most %d may be fitted", kind, MAX_CHIPS);
            return false;
        }

        spec = &out[n];
        spec->pin = devboard_pin(fields[0], errp);
        if (spec->pin < 0) {
            return false;
        }
        for (unsigned j = 0; j < n; j++) {
            if (out[j].pin == spec->pin) {
                error_setg(errp, "%s: %s is already taken by another",
                           kind, fields[0]);
                return false;
            }
        }
        spec->aux = -1;
        spec->count = 0;
        spec->name = g_strdup_printf("%s.%s", kind, fields[0]);

        if (fields[1] && *fields[1]) {
            if (aux_is_pin) {
                spec->aux = devboard_pin(fields[1], errp);
                if (spec->aux < 0) {
                    return false;
                }
            } else {
                unsigned long count;

                if (qemu_strtoul(fields[1], NULL, 10, &count) < 0 || !count) {
                    error_setg(errp, "%s: '%s' is not a length", kind,
                               fields[1]);
                    return false;
                }
                spec->count = count;
            }
        }
        n++;
    }

    *n_out = n;
    return true;
}

/*
 * Hands out a second consumer for a port line. The first goes straight on the
 * output; anything after that inserts a splitter, because a silently replaced
 * connection is invisible until some chip stops answering.
 */
static void devboard_drive(PIC16DevboardState *m, DeviceState *port,
                           int line, qemu_irq sink)
{
    DeviceState *split;

    if (!m->extra[line]) {
        qdev_connect_gpio_out(port, line, sink);
        m->extra[line] = sink;
        return;
    }

    split = qdev_new(TYPE_SPLIT_IRQ);
    qdev_prop_set_uint32(split, "num-lines", 2);
    qdev_realize_and_unref(split, NULL, &error_abort);
    qdev_connect_gpio_out(split, 0, m->extra[line]);
    qdev_connect_gpio_out(split, 1, sink);
    qdev_connect_gpio_out(port, line, qdev_get_gpio_in(split, 0));
    m->extra[line] = qdev_get_gpio_in(split, 0);
}

/*
 * A latched frame, on its way to the model outside. The strip decodes the
 * wire; turning pixels into a line of protocol is the bridge's business, so
 * the run-length encoding lives here rather than in the chip. Most of a frame
 * is usually one run, and the runs cover the whole strip in order.
 */
static void devboard_leds(void *opaque, WS2812State *strip,
                          const uint32_t *rgb, unsigned n)
{
    g_autoptr(GString) summary = g_string_new(strip->name);
    unsigned i = 0;

    while (i < n) {
        unsigned run = 1;

        while (i + run < n && rgb[i + run] == rgb[i]) {
            run++;
        }
        g_string_append_printf(summary, " %ux%06X", run, rgb[i]);
        i += run;
    }

    pic16_sim_bridge_send_event(opaque, "LEDS", summary->str);
}

/*
 * The two things that decide what the radio is transmitting: the NCO
 * increment, rewritten once per audio sample, which is the intermediate
 * frequency; and the ADF4002's counters, which are the local oscillator it is
 * mixed against. Together they are the signal, and rf-dump writes them out
 * with the virtual time each happened at so that something outside can
 * rebuild the waveform -- see scripts/xmas/.
 *
 * The dump is a file rather than a bridge event because there is one of these
 * per audio sample and the bridge is lock-step: a round trip each would cost
 * more than the emulation.
 */
static void G_GNUC_PRINTF(2, 3) devboard_rf_log(PIC16DevboardState *m,
                                                const char *fmt, ...)
{
    va_list ap;

    fprintf(m->rf_file, "%" PRId64 " ",
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    va_start(ap, fmt);
    vfprintf(m->rf_file, fmt, ap);
    va_end(ap);
    fputc('\n', m->rf_file);
}

static void devboard_nco_inc(void *opaque, uint32_t inc)
{
    PIC16DevboardState *m = opaque;

    if (m->rf_file) {
        devboard_rf_log(m, "nco inc=%05X", inc);
    }
    if (m->rf_bridge) {
        g_autofree char *str = g_strdup_printf("%05X", inc);

        pic16_sim_bridge_send_event(m->rf_bridge, "NCO", str);
    }
}

static void devboard_adf4002_latch(void *opaque, uint16_t r, uint16_t n,
                                   uint32_t func, bool locked)
{
    PIC16DevboardState *m = opaque;
    uint32_t lo = (uint32_t)n * 100000u;

    if (m->rf_file) {
        devboard_rf_log(m, "lo hz=%u r=%u n=%u func=%06X locked=%u",
                        lo, (unsigned)r, (unsigned)n, (unsigned)func,
                        (unsigned)locked);
        fflush(m->rf_file);
    }
    if (m->rf_bridge) {
        g_autofree char *str =
            g_strdup_printf("lo=%u r=%u n=%u func=%06X locked=%u",
                            lo, (unsigned)r, (unsigned)n, (unsigned)func,
                            (unsigned)locked);

        pic16_sim_bridge_send_event(m->rf_bridge, "ADF4002", str);
    }
}

static void devboard_init(MachineState *machine)
{
    PIC16DevboardState *m = PIC16_DEVBOARD_MACHINE(machine);
    DeviceState *port, *bridge = NULL;
    DeviceState *expander[MAX_CHIPS];
    DeviceState *pll = NULL;
    const char *soc_type = TYPE_PIC16F17546_SOC;
    bool is_28pin = false;
    unsigned i, j;

    if (!devboard_parse(m->expanders, "expander", true, m->expander,
                        &m->n_expanders, &error_fatal) ||
        !devboard_parse(m->leds, "led", false, m->led, &m->n_leds,
                        &error_fatal)) {
        exit(1);
    }

    if (m->soc_name && *m->soc_name) {
        if (!strcmp(m->soc_name, "pic16f15354") ||
            !strcmp(m->soc_name, "16f15354") ||
            !strcmp(m->soc_name, TYPE_PIC16F15354_SOC)) {
            soc_type = TYPE_PIC16F15354_SOC;
            is_28pin = true;
        } else if (!strcmp(m->soc_name, "pic16f15355") ||
                   !strcmp(m->soc_name, "16f15355") ||
                   !strcmp(m->soc_name, TYPE_PIC16F15355_SOC)) {
            soc_type = TYPE_PIC16F15355_SOC;
            is_28pin = true;
        } else if (!strcmp(m->soc_name, "pic16f17546") ||
                   !strcmp(m->soc_name, "16f17546") ||
                   !strcmp(m->soc_name, TYPE_PIC16F17546_SOC)) {
            soc_type = TYPE_PIC16F17546_SOC;
            is_28pin = false;
        } else {
            error_report("pic16-devboard: unsupported soc '%s'", m->soc_name);
            exit(1);
        }
    }

    object_initialize_child(OBJECT(machine), "soc-core", &m->soc, soc_type);
    sysbus_realize(SYS_BUS_DEVICE(&m->soc), &error_abort);
    port = DEVICE(&m->soc.port);

    for (i = 0; i < m->n_expanders; i++) {
        /*
         * Every chip on the bus sees every byte and answers only while its own
         * select is asserted, so the bus address is just a name -- but it has
         * to be unique or the bus refuses the second chip.
         */
        expander[i] = qdev_new(TYPE_MCP23S08);
        qdev_prop_set_uint8(expander[i], "cs", i);
        qdev_realize_and_unref(expander[i], BUS(m->soc.mssp1.ssi),
                               &error_fatal);
        if (m->expander[i].aux >= 0) {
            qdev_connect_gpio_out_named(
                expander[i], MCP23S08_INT_GPIO, 0,
                qdev_get_gpio_in_named(port, PIC16_PORT_IN_GPIO,
                                       m->expander[i].aux));
        }
    }

    /* ADF4002 PLL synthesizer */
    if (m->adf4002 && *m->adf4002) {
        g_auto(GStrv) fields = g_strsplit(m->adf4002, FIELD_SEP, 3);
        int le_pin = -1, ce_pin = -1, mux_pin = -1;

        if (fields[0] && *fields[0]) {
            le_pin = devboard_pin(fields[0], &error_fatal);
        }
        if (fields[0] && fields[1] && *fields[1]) {
            ce_pin = devboard_pin(fields[1], &error_fatal);
        }
        if (fields[0] && fields[1] && fields[2] && *fields[2]) {
            mux_pin = devboard_pin(fields[2], &error_fatal);
        }

        pll = qdev_new(TYPE_ADF4002);
        SSIBus *spi_bus = m->soc.mssp2.ssi ?: m->soc.mssp1.ssi;
        qdev_realize_and_unref(pll, BUS(spi_bus), &error_fatal);

        if (le_pin >= 0) {
            devboard_drive(m, port, le_pin,
                           qdev_get_gpio_in_named(pll, ADF4002_LE_GPIO, 0));
        }
        if (ce_pin >= 0) {
            devboard_drive(m, port, ce_pin,
                           qdev_get_gpio_in_named(pll, ADF4002_CE_GPIO, 0));
        }
        if (mux_pin >= 0) {
            qdev_connect_gpio_out_named(
                pll, ADF4002_MUX_OUT_GPIO, 0,
                qdev_get_gpio_in_named(port, PIC16_PORT_IN_GPIO, mux_pin));
        }
    }

    /*
     * Without a second serial there is no external model, and the board is
     * just its chips with nothing attached to their pins -- which is a
     * perfectly good way to run firmware that does not need one.
     */
    if (serial_hd(1)) {
        object_initialize_child(OBJECT(machine), "bridge", &m->bridge,
                                TYPE_PIC16_SIM_BRIDGE);
        bridge = DEVICE(&m->bridge);
        qdev_prop_set_chr(bridge, "chardev", serial_hd(1));
        m->rf_bridge = &m->bridge;

        if (is_28pin) {
            for (i = 0; i < ARRAY_SIZE(devboard_pins_28pin); i++) {
                g_autofree char *name =
                    g_strdup_printf("soc.%s", devboard_pins_28pin[i].name);
                pic16_sim_bridge_add_line(&m->bridge, name);
            }
        } else {
            for (i = 0; i < ARRAY_SIZE(devboard_pins_20pin); i++) {
                g_autofree char *name =
                    g_strdup_printf("soc.%s", devboard_pins_20pin[i].name);
                pic16_sim_bridge_add_line(&m->bridge, name);
            }
        }

        for (i = 0; i < m->n_expanders; i++) {
            for (j = 0; j < MCP23S08_PINS; j++) {
                g_autofree char *name =
                    g_strdup_printf("%s.GP%u", m->expander[i].name, j);

                pic16_sim_bridge_add_line(&m->bridge, name);
            }
        }

        sysbus_realize(SYS_BUS_DEVICE(&m->bridge), &error_abort);

        /*
         * Each SoC line runs both ways: the chip's output reaches the bridge,
         * and the bridge can drive the chip's input. Which direction is live
         * at any moment is TRIS's business, inside the port model.
         */
        unsigned n_pins = is_28pin ? ARRAY_SIZE(devboard_pins_28pin)
                                   : ARRAY_SIZE(devboard_pins_20pin);
        for (i = 0; i < n_pins; i++) {
            int line = is_28pin ? devboard_pins_28pin[i].line
                                : devboard_pins_20pin[i].line;

            devboard_drive(m, port, line, qdev_get_gpio_in(bridge, i));
            qdev_connect_gpio_out(bridge, i,
                                  qdev_get_gpio_in_named(port,
                                                         PIC16_PORT_IN_GPIO,
                                                         line));
        }
        for (i = 0; i < m->n_expanders; i++) {
            unsigned base = n_pins + i * MCP23S08_PINS;

            for (j = 0; j < MCP23S08_PINS; j++) {
                qdev_connect_gpio_out(bridge, base + j,
                                      qdev_get_gpio_in_named(expander[i],
                                                             MCP23S08_IN_GPIO,
                                                             j));
            }
        }
    }

    /*
     * The link to whatever is driving this board's SPI port. The port's own
     * chardev is the other way of doing it, but that one has no clock: bytes
     * arrive when the host gets round to writing them. This one carries the
     * master's virtual time, and holds this guest back to it.
     */
    if (m->fm_link && *m->fm_link) {
        Chardev *chr = qemu_chr_find(m->fm_link);

        if (!chr) {
            error_report("fm-link: there is no chardev called '%s'",
                         m->fm_link);
            exit(1);
        }
        object_initialize_child(OBJECT(machine), "cosim-link", &m->link,
                                TYPE_PIC16_COSIM_LINK);
        qdev_prop_set_chr(DEVICE(&m->link), "chardev", chr);
        pic16_cosim_link_set_mssp(&m->link, &m->soc.mssp1);
        sysbus_realize(SYS_BUS_DEVICE(&m->link), &error_fatal);
    }

    if (m->rf_dump && *m->rf_dump) {
        m->rf_file = fopen(m->rf_dump, "w");
        if (!m->rf_file) {
            error_report("rf-dump: cannot write '%s': %s", m->rf_dump,
                         strerror(errno));
            exit(1);
        }
    }
    if (m->rf_file || m->rf_bridge) {
        pic16_nco_set_increment_sink(&m->soc.nco1, devboard_nco_inc, m);
        if (pll) {
            adf4002_set_latch_sink(ADF4002(pll), devboard_adf4002_latch, m);
        }
    }

    /*
     * The chip selects come last so they fan out from whatever the bridge
     * already took, rather than the other way round.
     */
    for (i = 0; i < m->n_expanders; i++) {
        devboard_drive(m, port, m->expander[i].pin,
                       qdev_get_gpio_in_named(expander[i], SSI_GPIO_CS, 0));
    }

    for (i = 0; i < m->n_leds; i++) {
        DeviceState *strip = qdev_new(TYPE_WS2812);

        qdev_prop_set_uint32(strip, "pixels", m->led[i].count ?: 1);
        qdev_prop_set_string(strip, "name", m->led[i].name);
        if (bridge) {
            ws2812_set_frame_sink(WS2812(strip), devboard_leds, bridge);
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(strip), &error_fatal);
        devboard_drive(m, port, m->led[i].pin,
                       qdev_get_gpio_in_named(strip, WS2812_IN_GPIO, 0));
    }

    if (machine->firmware) {
        if (!pic16_load_firmware(machine->firmware, &m->soc.flash)) {
            exit(1);
        }
        pic16_load_config_words(&m->soc.cpu);
    }
}

static char *devboard_get_soc(Object *obj, Error **errp)
{
    return g_strdup(PIC16_DEVBOARD_MACHINE(obj)->soc_name ?: "pic16f17546");
}

static void devboard_set_soc(Object *obj, const char *value, Error **errp)
{
    PIC16DevboardState *m = PIC16_DEVBOARD_MACHINE(obj);
    g_free(m->soc_name);
    m->soc_name = g_strdup(value);
}

static char *devboard_get_expanders(Object *obj, Error **errp)
{
    return g_strdup(PIC16_DEVBOARD_MACHINE(obj)->expanders ?: "");
}

static void devboard_set_expanders(Object *obj, const char *value,
                                   Error **errp)
{
    PIC16DevboardState *m = PIC16_DEVBOARD_MACHINE(obj);
    ChipSpec probe[MAX_CHIPS];
    unsigned n;

    if (!devboard_parse(value, "expander", true, probe, &n, errp)) {
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        g_free(probe[i].name);
    }
    g_free(m->expanders);
    m->expanders = g_strdup(value);
}

static char *devboard_get_leds(Object *obj, Error **errp)
{
    return g_strdup(PIC16_DEVBOARD_MACHINE(obj)->leds ?: "");
}

static void devboard_set_leds(Object *obj, const char *value, Error **errp)
{
    PIC16DevboardState *m = PIC16_DEVBOARD_MACHINE(obj);
    ChipSpec probe[MAX_CHIPS];
    unsigned n;

    if (!devboard_parse(value, "led", false, probe, &n, errp)) {
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        g_free(probe[i].name);
    }
    g_free(m->leds);
    m->leds = g_strdup(value);
}

static char *devboard_get_adf4002(Object *obj, Error **errp)
{
    return g_strdup(PIC16_DEVBOARD_MACHINE(obj)->adf4002 ?: "");
}

static void devboard_set_adf4002(Object *obj, const char *value, Error **errp)
{
    PIC16DevboardState *m = PIC16_DEVBOARD_MACHINE(obj);
    g_free(m->adf4002);
    m->adf4002 = g_strdup(value);
}

static char *devboard_get_rf_dump(Object *obj, Error **errp)
{
    return g_strdup(PIC16_DEVBOARD_MACHINE(obj)->rf_dump ?: "");
}

static void devboard_set_rf_dump(Object *obj, const char *value, Error **errp)
{
    PIC16DevboardState *m = PIC16_DEVBOARD_MACHINE(obj);
    g_free(m->rf_dump);
    m->rf_dump = g_strdup(value);
}

static char *devboard_get_fm_link(Object *obj, Error **errp)
{
    return g_strdup(PIC16_DEVBOARD_MACHINE(obj)->fm_link ?: "");
}

static void devboard_set_fm_link(Object *obj, const char *value, Error **errp)
{
    PIC16DevboardState *m = PIC16_DEVBOARD_MACHINE(obj);
    g_free(m->fm_link);
    m->fm_link = g_strdup(value);
}

static void devboard_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "PIC16 development board with simulation bridge and configurable chips";
    mc->init = devboard_init;
    mc->default_cpu_type = PIC16_CPU_TYPE_NAME("pic16f1");
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;

    object_class_property_add_str(oc, "soc",
                                  devboard_get_soc,
                                  devboard_set_soc);
    object_class_property_set_description(oc, "soc",
        "Microcontroller SoC model: pic16f17546 (default), pic16f15354 or "
        "pic16f15355.");
    object_class_property_add_str(oc, "expanders",
                                  devboard_get_expanders,
                                  devboard_set_expanders);
    object_class_property_set_description(oc, "expanders",
        "MCP23S08 expanders as chip-select[:interrupt] separated by '/', "
        "e.g. RC7:RA2/RC6. None by default.");
    object_class_property_add_str(oc, "leds",
                                  devboard_get_leds, devboard_set_leds);
    object_class_property_set_description(oc, "leds",
        "WS2812 strips as data-pin[:pixels] separated by '/', "
        "e.g. RB7:237/RA4:12. None by default.");
    object_class_property_add_str(oc, "adf4002",
                                  devboard_get_adf4002,
                                  devboard_set_adf4002);
    object_class_property_set_description(oc, "adf4002",
        "ADF4002 PLL synthesizer as LE[:CE[:MUXOUT]] pin names, "
        "e.g. RC1:RC2:RA6. None by default.");
    object_class_property_add_str(oc, "rf-dump",
                                  devboard_get_rf_dump,
                                  devboard_set_rf_dump);
    object_class_property_set_description(oc, "rf-dump",
        "write the radio's own outputs -- every NCO increment and every "
        "ADF4002 latch -- to this file, timestamped in virtual nanoseconds");
    object_class_property_add_str(oc, "fm-link",
                                  devboard_get_fm_link,
                                  devboard_set_fm_link);
    object_class_property_set_description(oc, "fm-link",
        "chardev reaching the machine that drives this board's SPI port, "
        "which becomes this guest's clock master (see hw/chips/fm_link.c)");
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
