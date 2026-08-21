/*
 * Co-simulation link to the FM transmitter's own microcontroller
 *
 * The transmitter is not a chip with a data sheet: it is a PIC16 running a
 * firmware of its own, and hw/chips/fm_transmitter.c is a model of what that
 * firmware does. This is the alternative to modelling it -- the real thing,
 * in a second QEMU, wired to this one.
 *
 * The two are joined by a chardev carrying newline-delimited text. Everything
 * is stamped with this machine's virtual clock, and the slave follows it:
 *
 *   HELLO fm-link <version>          at reset, both ends, before anything else
 *   T <t_ns>                         virtual time has reached t_ns
 *   X <t_ns> <hex>                   a byte was clocked at t_ns
 *   Q <t_ns>                         the machine is going away
 *
 * and the slave answers a byte, and only a byte:
 *
 *   R <hex>                          what it clocked out in reply
 *
 * The exchange is blocking, which is the point: a transfer does not finish
 * until the slave has run its own guest up to the moment the byte arrived and
 * said what was in its transmit register when it did. Under -icount that
 * makes the pair a deterministic function of the two guests' execution, in
 * the same way hw/pic16/pic16_sim_bridge.c does for a physical model.
 *
 * SPI3 is timed on the wire (see pic32mk_soc.c), so the bytes are already
 * spaced by the time they really take, and the slave sees the same gaps the
 * silicon would. Between bytes the T messages keep the slave's clock within
 * "sync-ns" of this one, so its own sample timer drains its queue at the rate
 * this machine's frame timer fills it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "hw/chips/fm_link.h"

static void G_GNUC_PRINTF(2, 3) fm_link_fail(FMLinkState *s,
                                             const char *fmt, ...)
{
    va_list ap;

    if (s->failed) {
        return;
    }
    s->failed = true;

    va_start(ap, fmt);
    error_vreport(fmt, ap);
    va_end(ap);
    error_report("fm-link: giving up; the transmitter is no longer answering");
}

static void fm_link_send(FMLinkState *s, const char *line)
{
    g_autofree char *msg = g_strdup_printf("%s\n", line);

    if (s->failed) {
        return;
    }
    if (qemu_chr_fe_write_all(&s->chr, (const uint8_t *)msg,
                              strlen(msg)) < 0) {
        fm_link_fail(s, "fm-link: write failed");
    }
}

/* Blocking read of one newline-terminated message. */
static char *fm_link_recv(FMLinkState *s)
{
    for (;;) {
        uint8_t ch;
        int got;

        if (s->rx->len) {
            char *nl = memchr(s->rx->str, '\n', s->rx->len);

            if (nl) {
                gsize used = nl - s->rx->str;
                char *out = g_strndup(s->rx->str, used);

                g_string_erase(s->rx, 0, used + 1);
                return out;
            }
        }

        got = qemu_chr_fe_read_all(&s->chr, &ch, 1);
        if (got <= 0) {
            fm_link_fail(s, "fm-link: the transmitter disconnected");
            return NULL;
        }
        g_string_append_c(s->rx, ch);
    }
}

static void G_GNUC_PRINTF(2, 3) fm_link_log(FMLinkState *s,
                                            const char *fmt, ...)
{
    va_list ap;

    if (!s->dump_file) {
        return;
    }
    fprintf(s->dump_file, "%" PRId64 " ",
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    va_start(ap, fmt);
    vfprintf(s->dump_file, fmt, ap);
    va_end(ap);
    fputc('\n', s->dump_file);
    fflush(s->dump_file);
}

static void fm_link_lvl_apply(FMLinkState *s, uint8_t lvl)
{
    if (lvl == s->lvl) {
        return;
    }
    s->lvl = lvl;
    qemu_set_irq(s->lvl_out[0], lvl & 1);
    qemu_set_irq(s->lvl_out[1], (lvl >> 1) & 1);
    fm_link_log(s, "lvl %u%u", (lvl >> 1) & 1, lvl & 1);
}

/*
 * Asks the slave what its queue level lines show. Out-of-band state has no
 * byte exchange to ride on, and the anchors the master's pacing takes off
 * the lines are timestamped at arrival, so this rides the sync heartbeat:
 * the sampling error is bounded by sync-ns (100 us, a few samples), inside
 * the noise the master's estimator already filters.
 */
static void fm_link_lvl_poll(FMLinkState *s, int64_t now)
{
    g_autofree char *msg = g_strdup_printf("P %" PRId64, now);
    g_autofree char *reply = NULL;

    fm_link_send(s, msg);
    reply = fm_link_recv(s);
    if (!reply) {
        return;
    }
    if (reply[0] != 'L' || reply[1] != ' ') {
        fm_link_fail(s, "fm-link: expected the level lines, got '%s'", reply);
        return;
    }
    fm_link_lvl_apply(s, strtoul(reply + 2, NULL, 16) & 3);
}

/*
 * Tells the slave the time. It may not run past the last moment it has been
 * told about, so this is what lets it run at all while nothing is being sent
 * to it -- and what bounds how far the two clocks can drift apart. The level
 * lines ride along (fm_link_lvl_poll).
 */
static void fm_link_sync(void *opaque)
{
    FMLinkState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    g_autofree char *msg = g_strdup_printf("T %" PRId64, now);

    if (s->failed) {
        return;
    }
    fm_link_send(s, msg);
    fm_link_lvl_poll(s, now);
    timer_mod(s->sync, now + (int64_t)s->sync_ns);
}

static uint32_t fm_link_transfer(SSIPeripheral *dev, uint32_t val)
{
    FMLinkState *s = FM_LINK(dev);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t out = 0xFF;
    g_autofree char *msg = NULL;

    if (s->failed || !s->started) {
        return out;
    }

    msg = g_strdup_printf("X %" PRId64 " %02X", now, val & 0xFF);
    fm_link_send(s, msg);

    {
        g_autofree char *reply = fm_link_recv(s);

        if (!reply) {
            return out;
        }
        if (reply[0] != 'R' || reply[1] != ' ') {
            fm_link_fail(s, "fm-link: expected a reply byte, got '%s'", reply);
            return out;
        }
        out = strtoul(reply + 2, NULL, 16) & 0xFF;
    }

    /*
     * The slave has now run to this instant, so its clock is here too and the
     * next sync can wait a full period.
     */
    timer_mod(s->sync, now + (int64_t)s->sync_ns);

    s->last_rx = out;
    fm_link_log(s, "spi tx=%02X rx=%02X", val & 0xFF, out);
    return out;
}

static int fm_link_set_cs(SSIPeripheral *dev, bool cs)
{
    FMLinkState *s = FM_LINK(dev);
    bool selected = !cs;        /* the line high is the chip deselected */

    if (selected != s->selected) {
        s->selected = selected;
        fm_link_log(s, "cs %u", selected);
    }
    return 0;
}

static void fm_link_realize(SSIPeripheral *dev, Error **errp)
{
    FMLinkState *s = FM_LINK(dev);

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        error_setg(errp, "fm-link needs a chardev to reach the transmitter");
        return;
    }
    if (!s->sync_ns) {
        error_setg(errp, "fm-link: sync-ns must not be zero");
        return;
    }
    if (s->dump_path) {
        s->dump_file = fopen(s->dump_path, "w");
        if (!s->dump_file) {
            error_setg_errno(errp, errno, "failed to open FM link dump file "
                             "'%s'", s->dump_path);
            return;
        }
    }

    s->rx = g_string_new(NULL);
    s->sync = timer_new_ns(QEMU_CLOCK_VIRTUAL, fm_link_sync, s);
    qdev_init_gpio_out_named(DEVICE(s), s->lvl_out, FM_LINK_LVL_GPIO, 2);
}

/*
 * Both ends announce themselves and then read the other's announcement, so
 * neither waits for a message that has not been sent. A version that does not
 * match is a pair built from different trees, which is worth saying plainly
 * rather than discovering as a stuck transfer.
 */
static void fm_link_handshake(FMLinkState *s)
{
    g_autofree char *hello = NULL;
    g_autofree char *reply = NULL;

    const char *version;

    hello = g_strdup_printf("HELLO %s %d", TYPE_FM_LINK, FM_LINK_VERSION);
    fm_link_send(s, hello);

    reply = fm_link_recv(s);
    if (!reply) {
        return;
    }
    version = strrchr(reply, ' ');
    if (strncmp(reply, "HELLO ", 6) || !version ||
        atoi(version + 1) != FM_LINK_VERSION) {
        fm_link_fail(s, "fm-link: expected a version %d greeting, got '%s'",
                     FM_LINK_VERSION, reply);
        return;
    }
    s->started = true;
}

static void fm_link_reset_hold(Object *obj, ResetType type)
{
    FMLinkState *s = FM_LINK(obj);

    s->selected = false;
    s->last_rx = 0xFF;
    s->lvl = 0;

    if (!s->started && !s->failed) {
        fm_link_handshake(s);
    }
    if (s->started && !s->failed) {
        fm_link_sync(s);
    }
}

static void fm_link_unrealize(DeviceState *dev)
{
    FMLinkState *s = FM_LINK(dev);

    if (s->started && !s->failed) {
        g_autofree char *msg =
            g_strdup_printf("Q %" PRId64,
                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

        fm_link_send(s, msg);
    }
    if (s->dump_file) {
        fclose(s->dump_file);
        s->dump_file = NULL;
    }
    if (s->rx) {
        g_string_free(s->rx, TRUE);
        s->rx = NULL;
    }
}

static const Property fm_link_properties[] = {
    DEFINE_PROP_CHR("chardev", FMLinkState, chr),
    DEFINE_PROP_STRING("dump", FMLinkState, dump_path),
    DEFINE_PROP_UINT64("sync-ns", FMLinkState, sync_ns, 100000),
};

static const VMStateDescription fm_link_vmstate = {
    .name = "fm-link",
    .unmigratable = 1,
};

static void fm_link_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    k->realize = fm_link_realize;
    k->transfer = fm_link_transfer;
    k->set_cs = fm_link_set_cs;
    k->cs_polarity = SSI_CS_LOW;

    dc->unrealize = fm_link_unrealize;
    dc->desc = "co-simulation link to the FM transmitter's microcontroller";
    dc->vmsd = &fm_link_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, fm_link_properties);
    rc->phases.hold = fm_link_reset_hold;
}

static const TypeInfo fm_link_types[] = {
    {
        .name = TYPE_FM_LINK,
        .parent = TYPE_SSI_PERIPHERAL,
        .instance_size = sizeof(FMLinkState),
        .class_init = fm_link_class_init,
    },
};

DEFINE_TYPES(fm_link_types)
