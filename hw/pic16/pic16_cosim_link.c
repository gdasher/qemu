/*
 * The slave end of the FM co-simulation link
 *
 * The other end is hw/chips/fm_link.c, in the QEMU running the PIC32 that
 * drives this one. That machine is the clock master: it stamps everything it
 * sends with its own virtual time, and this end never lets its guest run past
 * the latest moment it has been told about. So the two guests advance
 * together, a byte reaches this firmware at the same virtual instant it left
 * the other, and the reply is whatever the firmware really had in SSP1BUF
 * when it did -- including nothing, which is how a master that clocks too
 * fast finds out.
 *
 * Blocking a vCPU on a chardev from a timer callback is what
 * hw/pic16/pic16_sim_bridge.c does for a physical model; the same argument
 * applies here, and for the same reason: lock-step is exactly what the
 * chardev callback interface does not give.
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
#include "migration/vmstate.h"
#include "hw/chips/fm_link.h"
#include "pic16_cosim_link.h"

static void G_GNUC_PRINTF(2, 3) pic16_cosim_link_fail(PIC16CosimLink *l,
                                                      const char *fmt, ...)
{
    va_list ap;

    if (l->failed) {
        return;
    }
    l->failed = true;

    va_start(ap, fmt);
    error_vreport(fmt, ap);
    va_end(ap);
    error_report("pic16-cosim-link: giving up; the guest now runs on its own "
                 "clock and nothing more will reach its SPI port");
}

static void pic16_cosim_link_send(PIC16CosimLink *l, const char *line)
{
    g_autofree char *msg = g_strdup_printf("%s\n", line);

    if (l->failed) {
        return;
    }
    if (qemu_chr_fe_write_all(&l->chr, (const uint8_t *)msg,
                              strlen(msg)) < 0) {
        pic16_cosim_link_fail(l, "pic16-cosim-link: write failed");
    }
}

/* Blocking read of one newline-terminated message. */
static char *pic16_cosim_link_recv(PIC16CosimLink *l)
{
    for (;;) {
        uint8_t ch;
        int got;

        if (l->rx->len) {
            char *nl = memchr(l->rx->str, '\n', l->rx->len);

            if (nl) {
                gsize used = nl - l->rx->str;
                char *out = g_strndup(l->rx->str, used);

                g_string_erase(l->rx, 0, used + 1);
                return out;
            }
        }

        got = qemu_chr_fe_read_all(&l->chr, &ch, 1);
        if (got <= 0) {
            pic16_cosim_link_fail(l, "pic16-cosim-link: the master "
                                  "disconnected");
            return NULL;
        }
        g_string_append_c(l->rx, ch);
    }
}

/*
 * The master's select line, routed to the SS pin (RC6). The pin is low while
 * selected; the rising edge of a deselect is what the firmware's
 * interrupt-on-change turns into a parser reset (the transmitter protocol's
 * SS framing, PROTOCOL.md).
 */
static void pic16_cosim_link_set_ss(PIC16CosimLink *l, bool selected)
{
    qemu_set_irq(l->ss_out, selected ? 0 : 1);
}

/* One byte off the wire, and the byte the firmware had waiting for it. */
static void pic16_cosim_link_clock(PIC16CosimLink *l, uint8_t in)
{
    g_autofree char *msg = NULL;
    uint8_t out = 0xFF;

    if (l->mssp) {
        out = pic16_mssp_slave_transfer(l->mssp, in);
    }
    l->bytes++;
    msg = g_strdup_printf("R %02X", out);
    pic16_cosim_link_send(l, msg);
}

/*
 * The guest has reached the last moment the master allowed. Nothing may run
 * until the master says more, so read it -- delivering any byte whose moment
 * has come -- and stop as soon as a message pushes the gate into the future.
 */
static void pic16_cosim_link_gate(void *opaque)
{
    PIC16CosimLink *l = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (l->failed || l->done) {
        return;
    }

    if (l->pending && l->pending_ns <= now) {
        l->pending = false;
        if (l->pending_cs) {
            l->pending_cs = false;
            pic16_cosim_link_set_ss(l, l->pending_sel);
        } else {
            pic16_cosim_link_clock(l, l->pending_byte);
        }
    }

    while (!l->failed && !l->done) {
        g_autofree char *msg = NULL;
        g_auto(GStrv) words = NULL;
        unsigned count;
        int64_t when;

        if (l->gate_ns > now) {
            timer_mod(l->gate, l->gate_ns);
            return;
        }

        msg = pic16_cosim_link_recv(l);
        if (!msg) {
            return;
        }
        words = g_strsplit(msg, " ", -1);
        for (count = 0; words[count]; count++) {
            continue;
        }
        if (count < 2) {
            pic16_cosim_link_fail(l, "pic16-cosim-link: malformed message "
                                  "'%s'", msg);
            return;
        }
        when = strtoll(words[1], NULL, 10);
        /*
         * The master's clock only goes forward and it never stamps a message
         * with a moment this guest has already passed, so one that arrives
         * late is a bug in the pairing rather than something to model. Count
         * it, deliver it now, and let the driver report the count.
         */
        if (when < now) {
            l->late++;
            when = now;
        }
        if (l->gate_ns < when) {
            l->gate_ns = when;
        }

        if (!strcmp(words[0], "T")) {
            continue;
        } else if (!strcmp(words[0], "P")) {
            /* The master asking after the queue level lines. */
            g_autofree char *lvl = g_strdup_printf("L %X", l->lvl & 3);

            pic16_cosim_link_send(l, lvl);
            continue;
        } else if (!strcmp(words[0], "X")) {
            uint8_t byte;

            if (count < 3) {
                pic16_cosim_link_fail(l, "pic16-cosim-link: '%s' carries no "
                                      "byte", msg);
                return;
            }
            byte = strtoul(words[2], NULL, 16) & 0xFF;
            if (when <= now) {
                pic16_cosim_link_clock(l, byte);
            } else {
                /* Its moment has not come; run the guest up to it first. */
                l->pending = true;
                l->pending_ns = when;
                l->pending_byte = byte;
            }
        } else if (!strcmp(words[0], "C")) {
            bool sel;

            if (count < 3) {
                pic16_cosim_link_fail(l, "pic16-cosim-link: '%s' carries no "
                                      "state", msg);
                return;
            }
            sel = strtoul(words[2], NULL, 10) != 0;
            if (when <= now) {
                pic16_cosim_link_set_ss(l, sel);
            } else {
                /* Its moment has not come; run the guest up to it first. */
                l->pending = true;
                l->pending_cs = true;
                l->pending_ns = when;
                l->pending_sel = sel;
            }
        } else if (!strcmp(words[0], "Q")) {
            l->done = true;
            timer_del(l->gate);
            return;
        } else {
            pic16_cosim_link_fail(l, "pic16-cosim-link: unexpected '%s'",
                                  words[0]);
            return;
        }
    }
}

static void pic16_cosim_link_set_lvl(void *opaque, int line, int level)
{
    PIC16CosimLink *l = opaque;

    if (level) {
        l->lvl |= 1u << line;
    } else {
        l->lvl &= ~(1u << line);
    }
}

void pic16_cosim_link_set_mssp(PIC16CosimLink *l, PIC16MsspState *mssp)
{
    l->mssp = mssp;
    /*
     * Writing SSPxBUF now preloads the reply to the next externally clocked
     * byte instead of starting a transfer of its own, which is what the port
     * does when something outside is the master.
     */
    mssp->external = true;
}

static void pic16_cosim_link_realize(DeviceState *dev, Error **errp)
{
    PIC16CosimLink *l = PIC16_COSIM_LINK(dev);

    if (!qemu_chr_fe_backend_connected(&l->chr)) {
        error_setg(errp, "pic16-cosim-link needs a chardev to reach the "
                   "master");
        return;
    }

    l->rx = g_string_new(NULL);
    l->gate = timer_new_ns(QEMU_CLOCK_VIRTUAL, pic16_cosim_link_gate, l);
    qdev_init_gpio_in_named(dev, pic16_cosim_link_set_lvl, FM_LINK_LVL_GPIO, 2);
    qdev_init_gpio_out_named(dev, &l->ss_out, FM_LINK_SS_GPIO, 1);
}

static void pic16_cosim_link_handshake(PIC16CosimLink *l)
{
    g_autofree char *hello = NULL;
    g_autofree char *reply = NULL;
    const char *version;

    hello = g_strdup_printf("HELLO %s %d", TYPE_PIC16_COSIM_LINK,
                            FM_LINK_VERSION);
    pic16_cosim_link_send(l, hello);

    reply = pic16_cosim_link_recv(l);
    if (!reply) {
        return;
    }
    version = strrchr(reply, ' ');
    if (strncmp(reply, "HELLO ", 6) || !version ||
        atoi(version + 1) != FM_LINK_VERSION) {
        pic16_cosim_link_fail(l, "pic16-cosim-link: expected a version %d "
                              "greeting, got '%s'", FM_LINK_VERSION, reply);
        return;
    }
    l->started = true;
}

static void pic16_cosim_link_reset_hold(Object *obj, ResetType type)
{
    PIC16CosimLink *l = PIC16_COSIM_LINK(obj);

    /*
     * A reset the guest brought on itself -- the watchdog, a RESET
     * instruction, a stack fault -- resets the chip, not the wire. The
     * master's clock has not moved, the moment it has allowed this guest to
     * run to still stands, and a byte it has clocked out and is waiting on
     * is still on its way: it gets whatever the freshly reset port answers,
     * as it would in silicon. Forgetting the byte here would leave the
     * master blocked for a reply that never comes, and this end blocked
     * behind a gate the master will not open until it gets one.
     */
    if (l->started || l->failed) {
        return;
    }

    l->pending = false;
    l->pending_cs = false;
    l->gate_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /* Nothing selects the chip until the master says so. */
    pic16_cosim_link_set_ss(l, false);

    pic16_cosim_link_handshake(l);
    if (l->started) {
        /*
         * Nothing has been allowed yet, so the guest stops at once and waits
         * for the master to boot far enough to say what time it is.
         */
        timer_mod(l->gate, l->gate_ns);
    }
}

static void pic16_cosim_link_unrealize(DeviceState *dev)
{
    PIC16CosimLink *l = PIC16_COSIM_LINK(dev);

    if (l->late) {
        warn_report("pic16-cosim-link: %" PRIu64 " of %" PRIu64 " bytes "
                    "arrived stamped before this guest's own clock",
                    l->late, l->bytes);
    }
    if (l->rx) {
        g_string_free(l->rx, TRUE);
        l->rx = NULL;
    }
}

static const Property pic16_cosim_link_properties[] = {
    DEFINE_PROP_CHR("chardev", PIC16CosimLink, chr),
};

static const VMStateDescription pic16_cosim_link_vmstate = {
    .name = "pic16-cosim-link",
    .unmigratable = 1,
};

static void pic16_cosim_link_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic16_cosim_link_realize;
    dc->unrealize = pic16_cosim_link_unrealize;
    dc->vmsd = &pic16_cosim_link_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, pic16_cosim_link_properties);
    rc->phases.hold = pic16_cosim_link_reset_hold;
}

static const TypeInfo pic16_cosim_link_types[] = {
    {
        .name = TYPE_PIC16_COSIM_LINK,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC16CosimLink),
        .class_init = pic16_cosim_link_class_init,
    },
};

DEFINE_TYPES(pic16_cosim_link_types)
