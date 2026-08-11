/*
 * One-of-N demultiplexer on a signal line
 *
 * A '4051 or the like: one input, a few address lines, an active-low enable,
 * and the input appears on whichever output the address picks while the rest
 * sit low. A board uses one to bit-bang several LED strings from a single pin,
 * which is what the XMASNg board does with eight.
 *
 * Only the selected output moves, and changing the address takes the old one
 * low first. That matters here: an LED strip decides where a frame ends by how
 * long its line has been quiet, so an output left high by a controller that
 * switched away mid-bit would corrupt the next frame it was selected for.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/chips/led_demux.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

static void led_demux_update(LedDemuxState *s)
{
    unsigned i;

    if (s->trace) {
        vcd_trace_set(s->trace, s->sig_in, s->level);
        vcd_trace_set(s->trace, s->sig_select, s->address);
        vcd_trace_set(s->trace, s->sig_enable, s->disabled);
    }
    for (i = 0; i < s->outputs; i++) {
        bool driven = !s->disabled && i == s->address && s->level;

        if (s->trace) {
            vcd_trace_set(s->trace, s->sig_out[i], driven);
        }
        qemu_set_irq(s->out[i], driven);
    }
}

static void led_demux_set_in(void *opaque, int line, int level)
{
    LedDemuxState *s = opaque;

    s->level = level;
    led_demux_update(s);
}

static void led_demux_set_select(void *opaque, int line, int level)
{
    LedDemuxState *s = opaque;
    uint32_t bit = 1u << line;

    if (level) {
        s->address |= bit;
    } else {
        s->address &= ~bit;
    }
    led_demux_update(s);
}

static void led_demux_set_enable(void *opaque, int line, int level)
{
    LedDemuxState *s = opaque;

    /* Active low, so a high line is the part switched off. */
    s->disabled = level;
    led_demux_update(s);
}

void led_demux_set_trace(LedDemuxState *s, VcdTrace *t)
{
    unsigned i;

    s->sig_in = vcd_trace_add(t, "din", 1);
    s->sig_select = vcd_trace_add(t, "sel", MAX(s->selects, 1));
    s->sig_enable = vcd_trace_add(t, "oe_n", 1);
    for (i = 0; i < s->outputs; i++) {
        g_autofree char *name = g_strdup_printf("led%u", i);

        s->sig_out[i] = vcd_trace_add(t, name, 1);
    }
    s->trace = t;
}

static void led_demux_reset_hold(Object *obj, ResetType type)
{
    LedDemuxState *s = LED_DEMUX(obj);

    s->level = false;
    s->address = 0;
    /*
     * Disabled until something drives the enable line, which is what an
     * unconnected input with a pull-up does. A board that leaves it unwired
     * says so by not asking for one -- see realize.
     */
    led_demux_update(s);
}

static void led_demux_realize(DeviceState *dev, Error **errp)
{
    LedDemuxState *s = LED_DEMUX(dev);

    if (!s->outputs || s->outputs > LED_DEMUX_MAX_OUTPUTS) {
        error_setg(errp, "led-demux: outputs must be between 1 and %d",
                   LED_DEMUX_MAX_OUTPUTS);
        return;
    }
    s->selects = 0;
    while ((1u << s->selects) < s->outputs) {
        s->selects++;
    }

    qdev_init_gpio_in_named(dev, led_demux_set_in, LED_DEMUX_IN_GPIO, 1);
    qdev_init_gpio_in_named(dev, led_demux_set_select, LED_DEMUX_SELECT_GPIO,
                            MAX(s->selects, 1));
    qdev_init_gpio_in_named(dev, led_demux_set_enable, LED_DEMUX_ENABLE_GPIO,
                            1);
    qdev_init_gpio_out_named(dev, s->out, LED_DEMUX_OUT_GPIO, s->outputs);
}

static const Property led_demux_properties[] = {
    DEFINE_PROP_UINT32("outputs", LedDemuxState, outputs, 1),
    /*
     * Whether the part is switched off to begin with. A board that does not
     * wire the enable line wants it enabled, since nothing will ever drive it.
     */
    DEFINE_PROP_BOOL("disabled", LedDemuxState, disabled, false),
};

static const VMStateDescription led_demux_vmstate = {
    .name = "led-demux",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(level, LedDemuxState),
        VMSTATE_BOOL(disabled, LedDemuxState),
        VMSTATE_UINT32(address, LedDemuxState),
        VMSTATE_END_OF_LIST()
    }
};

static void led_demux_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = led_demux_realize;
    dc->vmsd = &led_demux_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, led_demux_properties);
    rc->phases.hold = led_demux_reset_hold;
}

static const TypeInfo led_demux_types[] = {
    {
        .name = TYPE_LED_DEMUX,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(LedDemuxState),
        .class_init = led_demux_class_init,
    },
};

DEFINE_TYPES(led_demux_types)
