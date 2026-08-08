/*
 * WS2812 addressable LED strip
 *
 * A strip is one wire. The controller shifts 24 bits per pixel out of a GPIO,
 * encoding each bit in the ratio of a high pulse to the low that follows, and
 * a long low latches the frame. This decodes that back into pixel colours and
 * hands each frame to whatever the board hung off it, because a watcher wants
 * to know what the strip is showing, not to re-derive it from edge timing.
 *
 * The decode is deliberately free of absolute times. The part itself compares
 * each high against a fixed threshold of its own, but writing that threshold
 * down here would tie the model to how much virtual time an instruction takes,
 * which is the -icount shift's business: a model carrying the data sheet's
 * nanoseconds decodes noise at the wrong shift and looks like a firmware bug.
 *
 * So the threshold is learned from the frame. A frame carrying both bit values
 * separates into two clusters of pulse width and the threshold is the midpoint;
 * a frame that is all one colour carries only one width and cannot say on its
 * own, so it reuses what the last mixed frame taught. Only before any mixed
 * frame has arrived does it fall back to half the bit period, which is right
 * for the dark frame that case almost always is.
 *
 * Half the period is not a good rule on its own, which is worth recording: a
 * one's high is 0.64 of the period in the data sheet's waveform, but a
 * bit-banging controller stretches the low with its loop overhead, and the
 * real firmware this was written for ends up at 0.46 -- every one decoding as
 * a zero, and the strip reported dark no matter what it was sent.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "hw/chips/ws2812.h"

/*
 * How long a low has to be, in bit periods, before the frame is taken as
 * finished. The part latches on a low of more than 50 us against a 1.25 us
 * bit, which is forty periods, and this stops just short of it.
 *
 * It has to be nearly the whole of that, not a comfortable few periods: a
 * controller that bit-bangs from a task gets interrupted between pixels, and
 * anything shorter than the interrupt it takes cuts the frame in two -- with
 * the second half decoded as a strip's worth of pixels starting again at zero,
 * which is exactly what the part would do and not at all what the guest meant.
 */
#define QUIET_PERIODS 32

/*
 * How far apart the widest and narrowest pulse in a frame have to be before
 * the frame is taken to carry both bit values. A one is twice a zero in the
 * data sheet's waveform and rather more than that from a bit-banger, while
 * pulses of the same value differ only by whatever jitter the controller has,
 * so anything between about 1.2 and 2 separates the two cases.
 */
#define MIXED_RATIO_NUM 3
#define MIXED_RATIO_DEN 2

void ws2812_set_frame_sink(WS2812State *s, WS2812FrameFn fn, void *opaque)
{
    s->frame = fn;
    s->frame_opaque = opaque;
}

/*
 * Turns the buffered pulse widths into colours. WS2812 order is GRB and the
 * most significant bit goes first; this reports RGB, because that is what
 * anyone reading the summary means by a colour.
 */
static void ws2812_latch(WS2812State *s)
{
    g_autofree uint32_t *rgb = NULL;
    uint32_t threshold, lo, hi;
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

    lo = hi = s->high_ns[0];
    for (i = 1; i < s->n_bits; i++) {
        lo = MIN(lo, s->high_ns[i]);
        hi = MAX(hi, s->high_ns[i]);
    }
    if (hi * MIXED_RATIO_DEN >= lo * MIXED_RATIO_NUM) {
        threshold = (lo + hi) / 2;
        s->learned_ns = threshold;
    } else {
        threshold = s->learned_ns ?: s->period_ns / 2;
    }

    pixels = s->n_bits / WS2812_BITS_PER_PIXEL;
    rgb = g_new0(uint32_t, pixels ?: 1);

    for (i = 0; i < pixels; i++) {
        const uint32_t *bit = &s->high_ns[i * WS2812_BITS_PER_PIXEL];
        uint32_t grb = 0;
        unsigned b;

        for (b = 0; b < WS2812_BITS_PER_PIXEL; b++) {
            grb = (grb << 1) | (bit[b] > threshold);
        }
        rgb[i] = s->rgb_order ? grb                       /* red sent first */
                              : (((grb & 0x00FF00) << 8) | /* red is second */
                                 ((grb & 0xFF0000) >> 8) | /* green first */
                                 (grb & 0x0000FF));
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

    if (pixels && s->frame) {
        s->frame(s->frame_opaque, s, rgb, pixels);
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
     * A power cycle abandons whatever was mid-flight on the wire, because the
     * controller will start its next frame from the beginning. It does not
     * take the strip dark: the pixels hold their last latched colour until
     * something sends them another frame, which is what real ones do when the
     * controller in front of them restarts.
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
    if (!s->order || !strcmp(s->order, "grb")) {
        s->rgb_order = false;
    } else if (!strcmp(s->order, "rgb")) {
        s->rgb_order = true;
    } else {
        error_setg(errp, "ws2812: order must be 'grb' or 'rgb'");
        return;
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
    DEFINE_PROP_STRING("name", WS2812State, name),
    DEFINE_PROP_STRING("order", WS2812State, order),
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
        VMSTATE_UINT32(learned_ns, WS2812State),
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
