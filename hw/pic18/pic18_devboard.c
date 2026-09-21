/*
 * PIC18F27Q43 development board
 *
 * A PIC18F27Q43, a simulation bridge carrying every package pin, and whatever
 * chips you say are wired to it. Nothing is fitted by default: the board is
 * described on the command line, and what the pins mean is entirely the
 * business of whatever connects to the bridge.
 *
 *   -M pic18-devboard,adf4002=RC1:RC2:RA6,ad9834=RA3,rf-dump=rf.txt
 *
 * fits the XMASNGFMv2 rev 1.1 radio: an ADF4002 PLL on SPI2 with LE on RC1,
 * CE on RC2 and MUXOUT read back on RA6, and an AD9834 DDS on the same bus
 * with its FSYNC on RA3 -- which the firmware drives from SPI2's hardware
 * SS(out) through PPS, so the pin follows the module rather than LATA3.
 * "-M xmasngfm2" is that board with both chips preset.
 *
 * Serial 0 is the byte stream from whatever masters SPI1 (the radio's host
 * link), serial 1 the simulation bridge. The rf-dump file and the bridge both
 * receive the radio's own outputs: every AD9834 frequency update and every
 * ADF4002 latch.
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
#include "pic18q43_soc.h"
#include "hw/pic16/pic16_sim_bridge.h"
#include "hw/chips/fm_link.h"
#include "pic18_cosim_link.h"
#include "hw/chips/adf4002.h"
#include "hw/chips/ad9834.h"
#include "boot.h"

#define FIELD_SEP ":"

/* SS1, the host's select line, which the firmware routes to SPI1 through
 * SPI1SSPPS and watches with interrupt-on-change for the frame end. */
#define DEVBOARD_SS1_LINE (2 * PIC18_PORT_PINS + 6)     /* RC6 */
#define DEVBOARD_LVL0_LINE (2 * PIC18_PORT_PINS + 0)    /* RC0 */
#define DEVBOARD_LVL1_LINE (2 * PIC18_PORT_PINS + 7)    /* RC7 */

/* What a machine variant fits before the command line has its say. */
typedef struct {
    const char *desc;
    const char *adf4002;
    const char *ad9834;
} DevboardPreset;

struct PIC18DevboardMachineClass {
    MachineClass parent_class;
    const DevboardPreset *preset;
};
typedef struct PIC18DevboardMachineClass PIC18DevboardMachineClass;

struct PIC18DevboardState {
    MachineState parent_obj;

    PIC18Q43SocState soc;
    PIC16SimBridge bridge;
    PIC18CosimLink link;

    char *soc_name;
    int32_t osc_ppm;
    char *drift_profile;
    char *adf4002;
    char *ad9834;
    char *rf_dump;
    char *fm_link;

    /*
     * Where the radio's own outputs go. The bridge is one destination and the
     * rf-dump file is the other; a board can have both, so the sinks below
     * are given the machine rather than either of them.
     */
    PIC16SimBridge *rf_bridge;
    FILE *rf_file;
    AD9834State *dds;

    /*
     * A port line can have more than one consumer -- a chip select that the
     * model also watches, say -- and qdev_connect_gpio_out() replaces rather
     * than adds, so every extra consumer goes through a fan-out.
     */
    qemu_irq extra[PIC18_PORT_LINES];
};
typedef struct PIC18DevboardState PIC18DevboardState;

#define TYPE_PIC18_DEVBOARD_MACHINE MACHINE_TYPE_NAME("pic18-devboard")
DECLARE_OBJ_CHECKERS(PIC18DevboardState, PIC18DevboardMachineClass,
                     PIC18_DEVBOARD_MACHINE, TYPE_PIC18_DEVBOARD_MACHINE)

/* Package pins of the 28-pin PIC18F27Q43, by data sheet name and the port
 * model's line numbering. */
static const struct {
    const char *name;
    int line;
} devboard_pins[] = {
    { "RA0",  0 }, { "RA1",  1 }, { "RA2",  2 }, { "RA3",  3 },
    { "RA4",  4 }, { "RA5",  5 }, { "RA6",  6 }, { "RA7",  7 },
    { "RB0",  8 }, { "RB1",  9 }, { "RB2", 10 }, { "RB3", 11 },
    { "RB4", 12 }, { "RB5", 13 }, { "RB6", 14 }, { "RB7", 15 },
    { "RC0", 16 }, { "RC1", 17 }, { "RC2", 18 }, { "RC3", 19 },
    { "RC4", 20 }, { "RC5", 21 }, { "RC6", 22 }, { "RC7", 23 },
    { "RE3", PIC18_PORT_E * PIC18_PORT_PINS + 3 },  /* MCLR/VPP, input only */
};

static int devboard_pin(const char *name, Error **errp)
{
    if (strlen(name) == 3 && name[0] == 'R' &&
        ((name[1] >= 'A' && name[1] <= 'C') || name[1] == 'E') &&
        name[2] >= '0' && name[2] <= '7') {
        int port = name[1] == 'E' ? PIC18_PORT_E : name[1] - 'A';
        int pin = name[2] - '0';

        if (port == PIC18_PORT_E && pin != 3) {
            error_setg(errp, "'%s' is not bonded out on this package", name);
            return -1;
        }
        return port * PIC18_PORT_PINS + pin;
    }
    error_setg(errp, "'%s' is not a valid pin name on this package", name);
    return -1;
}

/*
 * Hands out a second consumer for a port line. The first goes straight on the
 * output; anything after that inserts a splitter, because a silently replaced
 * connection is invisible until some chip stops answering.
 */
static void devboard_drive(PIC18DevboardState *m, DeviceState *port,
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
 * An input pin can have more than one listener too: the SS1 line goes to the
 * port (for interrupt-on-change) and to SPI1's select input (for EOSIF).
 */
static qemu_irq devboard_fan_in(qemu_irq a, qemu_irq b)
{
    DeviceState *split = qdev_new(TYPE_SPLIT_IRQ);

    qdev_prop_set_uint32(split, "num-lines", 2);
    qdev_realize_and_unref(split, NULL, &error_abort);
    qdev_connect_gpio_out(split, 0, a);
    qdev_connect_gpio_out(split, 1, b);
    return qdev_get_gpio_in(split, 0);
}

/*
 * The two things that decide what the radio is transmitting: the AD9834's
 * frequency word, rewritten once per audio sample, which is the intermediate
 * frequency; and the ADF4002's counters, which are the local oscillator it is
 * mixed against. Together they are the signal, and rf-dump writes them out
 * with the virtual time each happened at so that something outside can
 * rebuild the waveform -- see scripts/xmas/.
 *
 * The dump is a file rather than a bridge event because there is one of these
 * per audio sample and the bridge is lock-step: a round trip each would cost
 * more than the emulation.
 */
static void G_GNUC_PRINTF(2, 3) devboard_rf_log(PIC18DevboardState *m,
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

static void devboard_dds_latch(void *opaque, uint32_t ftw, unsigned reg,
                               uint32_t control)
{
    PIC18DevboardState *m = opaque;
    unsigned hz;

    switch (reg) {
    case AD9834_REG_FREQ0:
    case AD9834_REG_FREQ1:
        /* Only the selected register is the output; the other is a preset. */
        if (reg != ((control & AD9834_CTRL_FSEL) ? 1 : 0)) {
            return;
        }
        hz = (unsigned)ad9834_frequency_hz(m->dds);
        if (m->rf_file) {
            devboard_rf_log(m, "dds ftw=%07X hz=%u", ftw, hz);
        }
        if (m->rf_bridge) {
            g_autofree char *str = g_strdup_printf("%07X", ftw);

            pic16_sim_bridge_send_event(m->rf_bridge, "DDS", str);
        }
        break;
    case AD9834_REG_CONTROL:
        if (m->rf_file) {
            devboard_rf_log(m, "ddsctl ctrl=%04X", control);
            fflush(m->rf_file);
        }
        break;
    default:
        break;
    }
}

static void devboard_adf4002_latch(void *opaque, uint16_t r, uint16_t n,
                                   uint32_t func, bool locked)
{
    PIC18DevboardState *m = opaque;
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
    PIC18DevboardState *m = PIC18_DEVBOARD_MACHINE(machine);
    DeviceState *port, *bridge = NULL;
    DeviceState *pll = NULL, *dds = NULL;
    const char *soc_type = TYPE_PIC18F27Q43_SOC;
    unsigned i;

    if (m->soc_name && *m->soc_name) {
        if (!strcmp(m->soc_name, "pic18f27q43") ||
            !strcmp(m->soc_name, "18f27q43") ||
            !strcmp(m->soc_name, TYPE_PIC18F27Q43_SOC)) {
            soc_type = TYPE_PIC18F27Q43_SOC;
        } else {
            error_report("pic18-devboard: unsupported soc '%s'", m->soc_name);
            exit(1);
        }
    }

    object_initialize_child(OBJECT(machine), "soc-core", &m->soc, soc_type);
    qdev_prop_set_int32(DEVICE(&m->soc), "osc-ppm", m->osc_ppm);
    if (m->drift_profile && *m->drift_profile) {
        pic18q43_soc_parse_drift(&m->soc, m->drift_profile);
    }
    sysbus_realize(SYS_BUS_DEVICE(&m->soc), &error_abort);
    port = DEVICE(&m->soc.port);

    /* ADF4002 PLL synthesizer, on SPI2 */
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
        qdev_realize_and_unref(pll, BUS(m->soc.spi2.ssi), &error_fatal);

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
                qdev_get_gpio_in_named(port, PIC18_PORT_IN_GPIO, mux_pin));
        }
    }

    /*
     * AD9834 DDS, on SPI2 as well. Its FSYNC is the named pin's level, and
     * the pin's level is whatever the PPS routing puts there: SPI2's SS(out)
     * while RA3PPS = 0x36, LATA3 otherwise.
     */
    if (m->ad9834 && *m->ad9834) {
        int fsync_pin = devboard_pin(m->ad9834, &error_fatal);

        dds = qdev_new(TYPE_AD9834);
        qdev_prop_set_uint8(dds, "cs", 1);  /* a bus address; just unique */
        qdev_realize_and_unref(dds, BUS(m->soc.spi2.ssi), &error_fatal);
        m->dds = AD9834(dds);
        devboard_drive(m, port, fsync_pin,
                       qdev_get_gpio_in_named(dds, SSI_GPIO_CS, 0));
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

        for (i = 0; i < ARRAY_SIZE(devboard_pins); i++) {
            g_autofree char *name =
                g_strdup_printf("soc.%s", devboard_pins[i].name);

            pic16_sim_bridge_add_line(&m->bridge, name);
        }
        sysbus_realize(SYS_BUS_DEVICE(&m->bridge), &error_abort);

        /*
         * Each SoC line runs both ways: the chip's output reaches the bridge,
         * and the bridge can drive the chip's input. Which direction is live
         * at any moment is TRIS's business, inside the port model. The SS1
         * pin's input also reaches SPI1's select input.
         */
        for (i = 0; i < ARRAY_SIZE(devboard_pins); i++) {
            int line = devboard_pins[i].line;
            qemu_irq sink = qdev_get_gpio_in_named(port, PIC18_PORT_IN_GPIO,
                                                   line);

            devboard_drive(m, port, line, qdev_get_gpio_in(bridge, i));
            if (line == DEVBOARD_SS1_LINE) {
                sink = devboard_fan_in(sink,
                                      qdev_get_gpio_in_named(
                                          DEVICE(&m->soc.spi1),
                                          PIC18_SPI_SS_IN_GPIO, 0));
            }
            qdev_connect_gpio_out(bridge, i, sink);
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
        qemu_irq ss_sink;

        if (!chr) {
            error_report("fm-link: there is no chardev called '%s'",
                         m->fm_link);
            exit(1);
        }
        object_initialize_child(OBJECT(machine), "cosim-link", &m->link,
                                TYPE_PIC18_COSIM_LINK);
        qdev_prop_set_chr(DEVICE(&m->link), "chardev", chr);
        pic18_cosim_link_set_spi(&m->link, &m->soc.spi1);
        sysbus_realize(SYS_BUS_DEVICE(&m->link), &error_fatal);
        /* The queue level lines, LVL0 on RC0 and LVL1 on RC7, cross the
           link too (the master polls them on its sync heartbeat). */
        devboard_drive(m, port, DEVBOARD_LVL0_LINE,
                       qdev_get_gpio_in_named(DEVICE(&m->link),
                                              FM_LINK_LVL_GPIO, 0));
        devboard_drive(m, port, DEVBOARD_LVL1_LINE,
                       qdev_get_gpio_in_named(DEVICE(&m->link),
                                              FM_LINK_LVL_GPIO, 1));
        /* And so does the master's chip select, onto SS1 (RC6): the
           deselect's rising edge is what the firmware's interrupt-on-change
           turns into a parser reset (the SS framing in PROTOCOL.md). */
        ss_sink = devboard_fan_in(
            qdev_get_gpio_in_named(port, PIC18_PORT_IN_GPIO,
                                   DEVBOARD_SS1_LINE),
            qdev_get_gpio_in_named(DEVICE(&m->soc.spi1),
                                   PIC18_SPI_SS_IN_GPIO, 0));
        qdev_connect_gpio_out_named(DEVICE(&m->link), FM_LINK_SS_GPIO, 0,
                                    ss_sink);
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
        if (dds) {
            ad9834_set_latch_sink(AD9834(dds), devboard_dds_latch, m);
        }
        if (pll) {
            adf4002_set_latch_sink(ADF4002(pll), devboard_adf4002_latch, m);
        }
    }

    if (machine->firmware) {
        if (!pic18_load_firmware(machine->firmware, &m->soc.flash)) {
            exit(1);
        }
        pic18_load_config_bytes(&m->soc.cpu);
    }
}

static char *devboard_get_soc(Object *obj, Error **errp)
{
    return g_strdup(PIC18_DEVBOARD_MACHINE(obj)->soc_name ?: "pic18f27q43");
}

static void devboard_set_soc(Object *obj, const char *value, Error **errp)
{
    PIC18DevboardState *m = PIC18_DEVBOARD_MACHINE(obj);

    g_free(m->soc_name);
    m->soc_name = g_strdup(value);
}

static char *devboard_get_adf4002(Object *obj, Error **errp)
{
    return g_strdup(PIC18_DEVBOARD_MACHINE(obj)->adf4002 ?: "");
}

static void devboard_set_adf4002(Object *obj, const char *value, Error **errp)
{
    PIC18DevboardState *m = PIC18_DEVBOARD_MACHINE(obj);

    g_free(m->adf4002);
    m->adf4002 = g_strdup(value);
}

static char *devboard_get_ad9834(Object *obj, Error **errp)
{
    return g_strdup(PIC18_DEVBOARD_MACHINE(obj)->ad9834 ?: "");
}

static void devboard_set_ad9834(Object *obj, const char *value, Error **errp)
{
    PIC18DevboardState *m = PIC18_DEVBOARD_MACHINE(obj);

    if (value && *value && devboard_pin(value, errp) < 0) {
        return;
    }
    g_free(m->ad9834);
    m->ad9834 = g_strdup(value);
}

static char *devboard_get_rf_dump(Object *obj, Error **errp)
{
    return g_strdup(PIC18_DEVBOARD_MACHINE(obj)->rf_dump ?: "");
}

static void devboard_set_rf_dump(Object *obj, const char *value, Error **errp)
{
    PIC18DevboardState *m = PIC18_DEVBOARD_MACHINE(obj);

    g_free(m->rf_dump);
    m->rf_dump = g_strdup(value);
}

static char *devboard_get_fm_link(Object *obj, Error **errp)
{
    return g_strdup(PIC18_DEVBOARD_MACHINE(obj)->fm_link ?: "");
}

static void devboard_set_fm_link(Object *obj, const char *value, Error **errp)
{
    PIC18DevboardState *m = PIC18_DEVBOARD_MACHINE(obj);

    g_free(m->fm_link);
    m->fm_link = g_strdup(value);
}

static char *devboard_get_osc_ppm(Object *obj, Error **errp)
{
    return g_strdup_printf("%d", PIC18_DEVBOARD_MACHINE(obj)->osc_ppm);
}

static void devboard_set_osc_ppm(Object *obj, const char *value, Error **errp)
{
    int val;

    if (qemu_strtoi(value, NULL, 0, &val) < 0) {
        error_setg(errp, "Invalid osc-ppm value: %s", value);
        return;
    }
    PIC18_DEVBOARD_MACHINE(obj)->osc_ppm = val;
}

static char *devboard_get_drift_profile(Object *obj, Error **errp)
{
    return g_strdup(PIC18_DEVBOARD_MACHINE(obj)->drift_profile ?: "");
}

static void devboard_set_drift_profile(Object *obj, const char *value,
                                       Error **errp)
{
    PIC18DevboardState *m = PIC18_DEVBOARD_MACHINE(obj);

    g_free(m->drift_profile);
    m->drift_profile = g_strdup(value);
}

/* A variant's preset chips, applied before the command line's properties. */
static void devboard_instance_init(Object *obj)
{
    PIC18DevboardState *m = PIC18_DEVBOARD_MACHINE(obj);
    const DevboardPreset *preset =
        PIC18_DEVBOARD_MACHINE_GET_CLASS(obj)->preset;

    if (preset) {
        m->adf4002 = g_strdup(preset->adf4002);
        m->ad9834 = g_strdup(preset->ad9834);
    }
}

static void devboard_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "PIC18F27Q43 development board with simulation bridge and "
               "configurable chips";
    mc->init = devboard_init;
    mc->default_cpu_type = PIC18_CPU_TYPE_NAME("pic18q43");
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;

    object_class_property_add_str(oc, "soc",
                                  devboard_get_soc,
                                  devboard_set_soc);
    object_class_property_set_description(oc, "soc",
        "Microcontroller SoC model: pic18f27q43 (default).");
    object_class_property_add_str(oc, "osc-ppm",
                                  devboard_get_osc_ppm,
                                  devboard_set_osc_ppm);
    object_class_property_set_description(oc, "osc-ppm",
        "Initial oscillator frequency offset in parts per million (ppm), "
        "e.g. 15000 or -20000.");
    object_class_property_add_str(oc, "drift-profile",
                                  devboard_get_drift_profile,
                                  devboard_set_drift_profile);
    object_class_property_set_description(oc, "drift-profile",
        "Dynamic thermal/drift profile (e.g. 'thermal', 'linear', "
        "'realistic', or 'thermal_max=3000,tau=2000,linear=20').");
    object_class_property_add_str(oc, "adf4002",
                                  devboard_get_adf4002,
                                  devboard_set_adf4002);
    object_class_property_set_description(oc, "adf4002",
        "ADF4002 PLL synthesizer on SPI2 as LE[:CE[:MUXOUT]] pin names, "
        "e.g. RC1:RC2:RA6. None by default.");
    object_class_property_add_str(oc, "ad9834",
                                  devboard_get_ad9834,
                                  devboard_set_ad9834);
    object_class_property_set_description(oc, "ad9834",
        "AD9834 DDS on SPI2, selected by the named FSYNC pin, e.g. RA3. "
        "None by default.");
    object_class_property_add_str(oc, "rf-dump",
                                  devboard_get_rf_dump,
                                  devboard_set_rf_dump);
    object_class_property_set_description(oc, "rf-dump",
        "write the radio's own outputs -- every AD9834 frequency update and "
        "every ADF4002 latch -- to this file, timestamped in virtual "
        "nanoseconds");
    object_class_property_add_str(oc, "fm-link",
                                  devboard_get_fm_link,
                                  devboard_set_fm_link);
    object_class_property_set_description(oc, "fm-link",
        "chardev reaching the machine that drives this board's SPI port, "
        "which becomes this guest's clock master (see hw/chips/fm_link.c)");
}

/* A variant: the same board, described up front rather than on the command
 * line. The properties stay available to override the preset. */
static void devboard_variant_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PIC18DevboardMachineClass *pmc = PIC18_DEVBOARD_MACHINE_CLASS(oc);

    pmc->preset = data;
    mc->desc = pmc->preset->desc;
}

static const DevboardPreset xmasngfm2_preset = {
    .desc = "XMASNGFMv2 rev 1.1 FM transmitter: PIC18F27Q43, ADF4002 "
            "(RC1:RC2:RA6) and AD9834 (RA3) on SPI2",
    .adf4002 = "RC1:RC2:RA6",
    .ad9834 = "RA3",
};

static const DevboardPreset bare_preset = {
    .desc = "PIC18F27Q43, no chips fitted (a bare part with a bridge)",
    .adf4002 = "",
    .ad9834 = "",
};

static const TypeInfo devboard_types[] = {
    {
        .name = TYPE_PIC18_DEVBOARD_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(PIC18DevboardState),
        .instance_init = devboard_instance_init,
        .class_size = sizeof(PIC18DevboardMachineClass),
        .class_init = devboard_machine_class_init,
    },
    {
        .name = MACHINE_TYPE_NAME("pic18f27q43"),
        .parent = TYPE_PIC18_DEVBOARD_MACHINE,
        .class_data = &bare_preset,
        .class_init = devboard_variant_class_init,
    },
    {
        .name = MACHINE_TYPE_NAME("xmasngfm2"),
        .parent = TYPE_PIC18_DEVBOARD_MACHINE,
        .class_data = &xmasngfm2_preset,
        .class_init = devboard_variant_class_init,
    },
};

DEFINE_TYPES(devboard_types)
