/*
 * Simulation bridge: pin-level co-simulation with an external physical model
 *
 * Lets a machine's physical model live outside QEMU, in the repository that
 * owns the product, while QEMU keeps only the parts that are chips. The
 * protocol names package pins -- "soc.RA4", "expander.GP0" -- so nothing here
 * knows what any pin is for.
 *
 * Lock-step against the virtual clock: an event blocks the vCPU until the
 * model answers. Under -icount that makes a run a deterministic function of
 * the guest's own execution, which is the point. The chardev is driven with
 * blocking read_all/write_all rather than the usual callbacks, because
 * lock-step is exactly what the callback interface does not give.
 *
 * See target/pic16/SIM-BRIDGE.md for the protocol.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "pic16_sim_bridge.h"

#define PROTOCOL_VERSION 1

int pic16_sim_bridge_add_line(PIC16SimBridge *b, const char *name)
{
    assert(b->n_lines < PIC16_SIM_BRIDGE_MAX_LINES);
    b->names[b->n_lines] = g_strdup(name);
    return b->n_lines++;
}

static int pic16_sim_bridge_line(PIC16SimBridge *b, const char *name)
{
    unsigned i;

    for (i = 0; i < b->n_lines; i++) {
        if (!strcmp(b->names[i], name)) {
            return i;
        }
    }
    return -1;
}

static void pic16_sim_bridge_fail(PIC16SimBridge *b, const char *fmt, ...)
{
    va_list ap;

    if (b->failed) {
        return;
    }
    b->failed = true;

    va_start(ap, fmt);
    error_vreport(fmt, ap);
    va_end(ap);
    error_report("pic16-sim-bridge: giving up; the model is no longer driving "
                 "any pin");
}

static void pic16_sim_bridge_send(PIC16SimBridge *b, const char *line)
{
    g_autofree char *msg = g_strdup_printf("%s\n", line);

    if (qemu_chr_fe_write_all(&b->chr, (const uint8_t *)msg, strlen(msg)) < 0) {
        pic16_sim_bridge_fail(b, "pic16-sim-bridge: write failed");
    }
}

/* Blocking read of one newline-terminated message. */
static char *pic16_sim_bridge_recv(PIC16SimBridge *b)
{
    for (;;) {
        uint8_t ch;
        int got;

        if (b->rx->len) {
            char *nl = memchr(b->rx->str, '\n', b->rx->len);

            if (nl) {
                gsize used = nl - b->rx->str;
                char *out = g_strndup(b->rx->str, used);

                g_string_erase(b->rx, 0, used + 1);
                return out;
            }
        }

        got = qemu_chr_fe_read_all(&b->chr, &ch, 1);
        if (got <= 0) {
            pic16_sim_bridge_fail(b, "pic16-sim-bridge: model disconnected");
            return NULL;
        }
        g_string_append_c(b->rx, ch);
    }
}

/*
 * Applies one "SET name=level ..." and returns false if the model touched a
 * line it never claimed with DRIVE, which is how the two-drivers mistake is
 * caught at the source.
 */
static bool pic16_sim_bridge_apply_set(PIC16SimBridge *b, char **words,
                                       unsigned count)
{
    unsigned i;

    for (i = 1; i < count; i++) {
        g_auto(GStrv) kv = g_strsplit(words[i], "=", 2);
        int line;

        if (!kv[0] || !kv[1]) {
            pic16_sim_bridge_fail(b, "pic16-sim-bridge: malformed SET '%s'",
                                  words[i]);
            return false;
        }
        line = pic16_sim_bridge_line(b, kv[0]);
        if (line < 0) {
            pic16_sim_bridge_fail(b, "pic16-sim-bridge: no such line '%s'",
                                  kv[0]);
            return false;
        }
        if (!b->driven[line]) {
            pic16_sim_bridge_fail(b, "pic16-sim-bridge: '%s' was not claimed "
                                  "with DRIVE", kv[0]);
            return false;
        }
        qemu_set_irq(b->out[line], atoi(kv[1]) != 0);
    }
    return true;
}

/*
 * Sends one event and consumes the reply: any number of SET lines, then one
 * ACK, optionally carrying an absolute virtual time to be woken at.
 */
static void pic16_sim_bridge_exchange(PIC16SimBridge *b, const char *event)
{
    if (b->failed || !b->ready) {
        return;
    }

    pic16_sim_bridge_send(b, event);

    for (;;) {
        g_autofree char *reply = pic16_sim_bridge_recv(b);
        g_auto(GStrv) words = NULL;
        unsigned count;

        if (!reply) {
            return;
        }
        words = g_strsplit(reply, " ", -1);
        for (count = 0; words[count]; count++) {
            continue;
        }
        if (!count) {
            continue;
        }

        if (!strcmp(words[0], "SET")) {
            if (!pic16_sim_bridge_apply_set(b, words, count)) {
                return;
            }
        } else if (!strcmp(words[0], "ACK")) {
            if (count > 1) {
                timer_mod(b->deadline, strtoll(words[1], NULL, 10));
            }
            return;
        } else {
            pic16_sim_bridge_fail(b, "pic16-sim-bridge: expected SET or ACK, "
                                  "got '%s'", words[0]);
            return;
        }
    }
}

void pic16_sim_bridge_send_event(PIC16SimBridge *b, const char *verb,
                                 const char *rest)
{
    g_autofree char *msg =
        g_strdup_printf("%s %" PRId64 " %s", verb,
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), rest);

    pic16_sim_bridge_exchange(b, msg);
}

static void pic16_sim_bridge_deadline(void *opaque)
{
    PIC16SimBridge *b = opaque;
    g_autofree char *msg =
        g_strdup_printf("TICK %" PRId64,
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

    pic16_sim_bridge_exchange(b, msg);
}

/* Handshake: announce the line set, then take the model's subscription. */
static void pic16_sim_bridge_handshake(PIC16SimBridge *b)
{
    g_autoptr(GString) lines = g_string_new("LINES");
    unsigned i;

    for (i = 0; i < b->n_lines; i++) {
        g_string_append_printf(lines, " %s", b->names[i]);
    }

    pic16_sim_bridge_send(b, "HELLO " TYPE_PIC16_SIM_BRIDGE " "
                          G_STRINGIFY(PROTOCOL_VERSION));
    pic16_sim_bridge_send(b, lines->str);

    for (;;) {
        g_autofree char *reply = pic16_sim_bridge_recv(b);
        g_auto(GStrv) words = NULL;
        unsigned n;

        if (!reply) {
            return;
        }
        words = g_strsplit(reply, " ", -1);
        for (n = 0; words[n]; n++) {
            continue;
        }
        if (!n) {
            continue;
        }

        if (!strcmp(words[0], "HELLO")) {
            continue;
        }
        if (!strcmp(words[0], "WATCH")) {
            for (i = 1; i < n; i++) {
                g_auto(GStrv) kv = g_strsplit(words[i], "=", 2);
                int line = pic16_sim_bridge_line(b, kv[0]);

                if (line < 0) {
                    pic16_sim_bridge_fail(b, "pic16-sim-bridge: WATCH names "
                                          "unknown line '%s'", kv[0]);
                    return;
                }
                if (!kv[1] || !strcmp(kv[1], "both")) {
                    b->watch[line] = PIC16_WATCH_BOTH;
                } else if (!strcmp(kv[1], "rising")) {
                    b->watch[line] = PIC16_WATCH_RISING;
                } else if (!strcmp(kv[1], "falling")) {
                    b->watch[line] = PIC16_WATCH_FALLING;
                } else {
                    pic16_sim_bridge_fail(b, "pic16-sim-bridge: bad WATCH mode "
                                          "'%s'", kv[1]);
                    return;
                }
            }
        } else if (!strcmp(words[0], "DRIVE")) {
            for (i = 1; i < n; i++) {
                int line = pic16_sim_bridge_line(b, words[i]);

                if (line < 0) {
                    pic16_sim_bridge_fail(b, "pic16-sim-bridge: DRIVE names "
                                          "unknown line '%s'", words[i]);
                    return;
                }
                b->driven[line] = true;
            }
        } else if (!strcmp(words[0], "READY")) {
            b->ready = true;
            return;
        } else {
            pic16_sim_bridge_fail(b, "pic16-sim-bridge: unexpected '%s' during "
                                  "handshake", words[0]);
            return;
        }
    }
}

/* Every line and its level, so the model never has to guess at one. */
static void pic16_sim_bridge_send_state(PIC16SimBridge *b)
{
    g_autoptr(GString) state = g_string_new(NULL);
    unsigned i;

    g_string_printf(state, "STATE %" PRId64,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    for (i = 0; i < b->n_lines; i++) {
        g_string_append_printf(state, " %s=%u", b->names[i], b->level[i]);
    }
    pic16_sim_bridge_exchange(b, state->str);
}

/*
 * The chip drove one of its pins. The handshake is deferred to here rather
 * than done at realize, because the model may not have connected yet when the
 * machine is built.
 */
static void pic16_sim_bridge_set_line(void *opaque, int line, int level)
{
    PIC16SimBridge *b = opaque;
    g_autofree char *msg = NULL;
    bool rising;

    if (b->failed) {
        return;
    }
    if ((bool)level == (bool)b->level[line]) {
        return;
    }
    rising = level;
    b->level[line] = level;

    if (!b->ready) {
        pic16_sim_bridge_handshake(b);
        if (!b->ready) {
            return;
        }
        /* Give the model the whole starting picture before any edge. */
        pic16_sim_bridge_send_state(b);
    }

    if (!(b->watch[line] & (rising ? PIC16_WATCH_RISING
                                   : PIC16_WATCH_FALLING))) {
        return;
    }

    msg = g_strdup_printf("EDGE %" PRId64 " %s=%d",
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                          b->names[line], level ? 1 : 0);
    pic16_sim_bridge_exchange(b, msg);
}

/*
 * The board was power-cycled: a guest RESET, a stack fault or the watchdog.
 * Every pin goes back to being an input, which is no level at all, and this
 * models that as low. The ports do not re-drive anything on reset, so the
 * snapshot is what re-synchronises the model with them -- and it is the same
 * picture the model was given at the handshake, which is what makes a reset
 * indistinguishable from a fresh start.
 */
static void pic16_sim_bridge_reset_hold(Object *obj, ResetType type)
{
    PIC16SimBridge *b = PIC16_SIM_BRIDGE(obj);
    g_autofree char *msg = NULL;

    memset(b->level, 0, sizeof(b->level));

    if (!b->ready || b->failed) {
        return;
    }
    msg = g_strdup_printf("RESET %" PRId64,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    pic16_sim_bridge_exchange(b, msg);
    pic16_sim_bridge_send_state(b);
}

static void pic16_sim_bridge_realize(DeviceState *dev, Error **errp)
{
    PIC16SimBridge *b = PIC16_SIM_BRIDGE(dev);

    if (!qemu_chr_fe_backend_connected(&b->chr)) {
        error_setg(errp, "pic16-sim-bridge needs a chardev");
        return;
    }

    b->rx = g_string_new(NULL);
    b->deadline = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                               pic16_sim_bridge_deadline, b);

    qdev_init_gpio_in(dev, pic16_sim_bridge_set_line, b->n_lines);
    qdev_init_gpio_out(dev, b->out, b->n_lines);
}

static void pic16_sim_bridge_unrealize(DeviceState *dev)
{
    PIC16SimBridge *b = PIC16_SIM_BRIDGE(dev);
    unsigned i;

    if (b->ready && !b->failed) {
        pic16_sim_bridge_send(b, "BYE");
    }
    for (i = 0; i < b->n_lines; i++) {
        g_free(b->names[i]);
    }
    g_string_free(b->rx, TRUE);
}

static const Property pic16_sim_bridge_properties[] = {
    DEFINE_PROP_CHR("chardev", PIC16SimBridge, chr),
};

static void pic16_sim_bridge_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic16_sim_bridge_realize;
    dc->unrealize = pic16_sim_bridge_unrealize;
    dc->user_creatable = false;
    device_class_set_props(dc, pic16_sim_bridge_properties);
    rc->phases.hold = pic16_sim_bridge_reset_hold;
}

static const TypeInfo pic16_sim_bridge_types[] = {
    {
        .name = TYPE_PIC16_SIM_BRIDGE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC16SimBridge),
        .class_init = pic16_sim_bridge_class_init,
    },
};

DEFINE_TYPES(pic16_sim_bridge_types)
