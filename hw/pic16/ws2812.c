/*
 * WS2812 addressable LED strip
 *
 * A strip is one wire. The controller shifts 24 bits per pixel out of a GPIO,
 * encoding each bit in the ratio of a high pulse to the low that follows, and
 * a long low latches the frame. This decodes that back into pixel colours and
 * reports each frame to the simulation bridge as a run-length summary --
 * "12x00FF00 4xFF0000" -- because a model outside QEMU wants to know what the
 * strip is showing, not to re-derive it from edge timing.
 *
 * The decode is deliberately free of absolute times. A bit's high is 0.32 of
 * its period for a zero and 0.64 for a one, so the threshold is half the
 * period, and the period is measured from the frame's own first bit. That
 * makes it independent of the -icount shift, which sets how fast guest
 * instructions retire in virtual time and therefore how wide a bit-banged
 * pulse comes out. A model with the data sheet's nanoseconds in it would
 * decode nothing but noise at the wrong shift, and would look like a firmware
 * bug.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "ws2812.h"

/*
 * How long a low has to be, in bit periods, before the frame is taken as
 * finished. The part latches on a low of more than 50 us against a 1.25 us
 * bit, so anything well above one period and below forty is the same
 * decision; four leaves room for a controller that dawdles between bytes.
 */
#define QUIET_PERIODS 4

static void ws2812_report(WS2812State *s, const uint32_t *rgb, unsigned n)
{
    g_autoptr(GString) summary = g_string_new(NULL);
    unsigned i = 0;

    g_string_append(summary, s->name);
    while (i < n) {
        unsigned run = 1;

        while (i + run < n && rgb[i + run] == rgb[i]) {
            run++;
        }
        g_string_append_printf(summary, " %ux%06X", run, rgb[i]);
        i += run;
    }

    if (s->bridge) {
        pic16_sim_bridge_send_event(s->bridge, "LEDS", summary->str);
    }
}

/*
 * Turns the buffered pulse widths into colours. WS2812 order is GRB and the
 * most significant bit goes first; this reports RGB, because that is what
 * anyone reading the summary means by a colour.
 */
static void ws2812_latch(WS2812State *s)
{
    g_autofree uint32_t *rgb = NULL;
    uint32_t threshold;
    unsigned pixels, i;

    if (s->quiet) {
        timer_del(s->quiet);
    }

    if (!s->n_bits) {
        return;
    }
    if (!s->period_ns) {
        /* One pulse and nothing after it: no period, so nothing to decode. */
        s->n_bits = 0;
        return;
    }

    threshold = s->period_ns / 2;
    pixels = s->n_bits / WS2812_BITS_PER_PIXEL;
    rgb = g_new0(uint32_t, pixels ?: 1);

    for (i = 0; i < pixels; i++) {
        const uint32_t *bit = &s->high_ns[i * WS2812_BITS_PER_PIXEL];
        uint32_t grb = 0;
        unsigned b;

        for (b = 0; b < WS2812_BITS_PER_PIXEL; b++) {
            grb = (grb << 1) | (bit[b] > threshold);
        }
        rgb[i] = ((grb & 0x00FF00) << 8) |    /* red, the middle byte */
                 ((grb & 0xFF0000) >> 8) |    /* green, sent first */
                 (grb & 0x0000FF);
    }

    if (s->n_bits % WS2812_BITS_PER_PIXEL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ws2812: %s got %u bits, not a whole number of pixels; "
                      "reporting the %u complete ones\n",
                      s->name, s->n_bits, pixels);
    }
    if (s->overrun) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ws2812: %s is %u pixels long but the guest kept "
                      "shifting; the excess was dropped\n",
                      s->name, s->pixels);
    }

    if (pixels) {
        ws2812_report(s, rgb, pixels);
    }

    s->n_bits = 0;
    s->period_ns = 0;
    s->overrun = false;
}

static void ws2812_quiet(void *opaque)
{
    ws2812_latch(opaque);
}

static void ws2812_set_din(void *opaque, int line, int level)
{
    WS2812State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t width = now - s->edge_ns;

    if (!!level == s->level) {
        return;
    }
    s->level = level;

    if (!level) {
        /* The high just ended, and its width is the bit. */
        if (s->n_bits < s->pixels * WS2812_BITS_PER_PIXEL) {
            s->high_ns[s->n_bits++] = width;
        } else {
            s->overrun = true;
        }
        if (s->period_ns) {
            timer_mod(s->quiet, now + QUIET_PERIODS * (int64_t)s->period_ns);
        }
    } else {
        /*
         * The low just ended. If that closed the frame's first bit it also
         * fixes the period, which every later bit in this frame is measured
         * against. Before the first high, edge_ns is stale and the width is
         * meaningless, which is why n_bits gates this.
         */
        if (s->n_bits == 1 && !s->period_ns) {
            s->period_ns = s->high_ns[0] + width;
        }
        timer_del(s->quiet);
    }

    s->edge_ns = now;
}

static void ws2812_reset_hold(Object *obj, ResetType type)
{
    WS2812State *s = WS2812(obj);

    /*
     * A power cycle takes the strip dark and abandons whatever was mid-flight
     * on the wire; the controller starts its next frame from the beginning.
     */
    if (s->quiet) {
        timer_del(s->quiet);
    }
    s->n_bits = 0;
    s->period_ns = 0;
    s->overrun = false;
    s->level = false;
    s->edge_ns = 0;
}

static void ws2812_realize(DeviceState *dev, Error **errp)
{
    WS2812State *s = WS2812(dev);

    if (s->pixels == 0 || s->pixels > WS2812_MAX_PIXELS) {
        error_setg(errp, "ws2812: pixels must be between 1 and %u",
                   WS2812_MAX_PIXELS);
        return;
    }
    if (!s->name) {
        s->name = g_strdup(TYPE_WS2812);
    }

    s->quiet = timer_new_ns(QEMU_CLOCK_VIRTUAL, ws2812_quiet, s);
    qdev_init_gpio_in_named(dev, ws2812_set_din, WS2812_IN_GPIO, 1);
}

static void ws2812_unrealize(DeviceState *dev)
{
    WS2812State *s = WS2812(dev);

    timer_free(s->quiet);
}

static const Property ws2812_properties[] = {
    DEFINE_PROP_UINT32("pixels", WS2812State, pixels, 1),
    DEFINE_PROP_STRING("bridge-name", WS2812State, name),
    DEFINE_PROP_LINK("bridge", WS2812State, bridge, TYPE_PIC16_SIM_BRIDGE,
                     PIC16SimBridge *),
};

static const VMStateDescription ws2812_vmstate = {
    .name = "ws2812",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(high_ns, WS2812State,
                             WS2812_MAX_PIXELS * WS2812_BITS_PER_PIXEL),
        VMSTATE_UINT32(n_bits, WS2812State),
        VMSTATE_UINT32(period_ns, WS2812State),
        VMSTATE_BOOL(overrun, WS2812State),
        VMSTATE_BOOL(level, WS2812State),
        VMSTATE_INT64(edge_ns, WS2812State),
        VMSTATE_END_OF_LIST()
    }
};

static void ws2812_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = ws2812_realize;
    dc->unrealize = ws2812_unrealize;
    dc->vmsd = &ws2812_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, ws2812_properties);
    rc->phases.hold = ws2812_reset_hold;
}

static const TypeInfo ws2812_types[] = {
    {
        .name = TYPE_WS2812,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(WS2812State),
        .class_init = ws2812_class_init,
    },
};

DEFINE_TYPES(ws2812_types)
