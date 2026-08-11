/*
 * A view of what a set of WS2812 strips is showing
 *
 * Each strip becomes a row of pixels, so a machine driving eight strings of
 * six hundred LEDs shows up as an 8 x 600 image and a movie plays in a window.
 * The same frames go to a file when one is asked for, run-length encoded, one
 * line per strip per frame:
 *
 *     4123456 led0 12x00FF00 588x000000
 *
 * That file is what a test compares against, which is why it carries the time
 * a frame latched: a movie that plays at the wrong rate is a real failure and
 * an image comparison would not see it.
 *
 * A board may also hang on/off outputs here -- relays, on this one -- which
 * are not pixels and are drawn as blocks under the rows rather than as part of
 * them. They reach the same file, as the moment one moved:
 *
 *     4123456 switches 05
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/ws2812_panel.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/surface.h"

static unsigned ws2812_panel_index(WS2812PanelState *s, WS2812State *strip)
{
    unsigned i;

    for (i = 0; i < s->n_strips; i++) {
        if (s->strip[i] == strip) {
            return i;
        }
    }
    return 0;
}

/* Writes the switch line being held, if the clock has left it behind. */
static void ws2812_panel_flush_switches(WS2812PanelState *s, int64_t now)
{
    if (!s->switch_pending || (now >= 0 && now == s->switch_time)) {
        return;
    }
    fprintf(s->dump_file, "%" PRId64 " switches %02X\n",
            s->switch_time, s->switch_state);
    fflush(s->dump_file);
    s->switch_pending = false;
}

static void ws2812_panel_write_dump(WS2812PanelState *s, unsigned index,
                                    const uint32_t *rgb, unsigned pixels)
{
    unsigned i = 0;

    ws2812_panel_flush_switches(s, -1);

    fprintf(s->dump_file, "%" PRId64 " led%u",
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), index);
    while (i < pixels) {
        unsigned run = 1;

        while (i + run < pixels && rgb[i + run] == rgb[i]) {
            run++;
        }
        fprintf(s->dump_file, " %ux%06X", run, rgb[i]);
        i += run;
    }
    fputc('\n', s->dump_file);
    fflush(s->dump_file);
}

static void ws2812_panel_frame(void *opaque, WS2812State *strip,
                               const uint32_t *rgb, unsigned pixels)
{
    WS2812PanelState *s = opaque;
    unsigned index = ws2812_panel_index(s, strip);
    unsigned n = MIN(pixels, s->pixels);

    memcpy(&s->rgb[index * s->pixels], rgb, n * sizeof(*rgb));
    s->dirty = true;

    if (s->dump_file) {
        ws2812_panel_write_dump(s, index, rgb, pixels);
    }
}

void ws2812_panel_set_switch(WS2812PanelState *s, unsigned index, bool on)
{
    uint8_t bit = 1u << index;
    uint8_t was = s->switch_state;

    if (index >= s->switches) {
        return;
    }
    s->switch_state = on ? (was | bit) : (was & ~bit);
    if (s->switch_state == was) {
        return;
    }
    s->dirty = true;

    /*
     * A line when one moves, rather than one per frame. The firmware writes
     * the relays on every frame whether or not anything changed, and what a
     * reader wants is the moment something did: the state at any other is the
     * last line before it.
     */
    if (s->dump_file) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        ws2812_panel_flush_switches(s, now);
        s->switch_pending = true;
        s->switch_time = now;
    }
}

void ws2812_panel_add(WS2812PanelState *s, WS2812State *strip)
{
    if (s->n_strips == WS2812_PANEL_MAX_STRIPS) {
        return;
    }
    s->strip[s->n_strips++] = strip;
    ws2812_set_frame_sink(strip, ws2812_panel_frame, s);
}

static void ws2812_panel_invalidate(void *opaque)
{
    WS2812PanelState *s = opaque;

    s->dirty = true;
}

/* The block a switch is drawn as, and the gap around it, in screen pixels. */
static unsigned ws2812_panel_block(WS2812PanelState *s)
{
    return WS2812_PANEL_SWITCH_PIXELS * s->scale;
}

static unsigned ws2812_panel_band(WS2812PanelState *s)
{
    return s->switches ? ws2812_panel_block(s) * 2 : 0;
}

/*
 * The switches, as blocks under the strings: white for a closed relay, and an
 * outline for an open one, so an output that is off still says where it is.
 */
static void ws2812_panel_draw_switches(WS2812PanelState *s,
                                       DisplaySurface *surface)
{
    unsigned block = ws2812_panel_block(s);
    unsigned pad = block / 2;
    unsigned top = s->strips * s->scale + pad;
    unsigned i, y, x;

    for (y = 0; y < ws2812_panel_band(s) - pad; y++) {
        uint32_t *line = (uint32_t *)(surface_data(surface) +
                                      (top + y) * surface_stride(surface));

        for (x = 0; x < surface_width(surface); x++) {
            line[x] = 0;
        }
        for (i = 0; i < s->switches; i++) {
            unsigned left = pad + i * (block + pad);
            bool on = s->switch_state & (1u << i);
            bool edge = y == 0 || y == block - 1;

            if (y >= block || left + block > surface_width(surface)) {
                continue;
            }
            for (x = 0; x < block; x++) {
                bool side = x == 0 || x == block - 1;

                line[left + x] = on ? 0xFFFFFF
                                    : (edge || side ? 0x303030 : 0);
            }
        }
    }
}

static bool ws2812_panel_update(void *opaque)
{
    WS2812PanelState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    unsigned strip, pixel, y, x;
    uint32_t *line;

    if (!s->dirty || !surface) {
        return true;
    }
    s->dirty = false;

    for (strip = 0; strip < s->strips; strip++) {
        for (y = 0; y < s->scale; y++) {
            line = (uint32_t *)(surface_data(surface) +
                                (strip * s->scale + y) *
                                surface_stride(surface));
            for (pixel = 0; pixel < s->pixels; pixel++) {
                uint32_t colour = s->rgb[strip * s->pixels + pixel];

                for (x = 0; x < s->scale; x++) {
                    line[pixel * s->scale + x] = colour;
                }
            }
        }
    }

    if (s->switches) {
        ws2812_panel_draw_switches(s, surface);
    }
    qemu_console_update_full(s->con);
    return true;
}

static const GraphicHwOps ws2812_panel_ops = {
    .invalidate = ws2812_panel_invalidate,
    .gfx_update = ws2812_panel_update,
};

static void ws2812_panel_realize(DeviceState *dev, Error **errp)
{
    WS2812PanelState *s = WS2812_PANEL(dev);

    if (!s->strips || s->strips > WS2812_PANEL_MAX_STRIPS) {
        error_setg(errp, "ws2812-panel: strips must be between 1 and %d",
                   WS2812_PANEL_MAX_STRIPS);
        return;
    }
    if (!s->pixels || !s->scale) {
        error_setg(errp, "ws2812-panel: needs a length and a scale");
        return;
    }

    if (s->switches > WS2812_PANEL_MAX_SWITCHES) {
        error_setg(errp, "ws2812-panel: at most %d switches",
                   WS2812_PANEL_MAX_SWITCHES);
        return;
    }

    if (s->dump && *s->dump) {
        s->dump_file = fopen(s->dump, "w");
        if (!s->dump_file) {
            error_setg_errno(errp, errno, "ws2812-panel: cannot write '%s'",
                             s->dump);
            return;
        }
    }

    s->rgb = g_new0(uint32_t, (size_t)s->strips * s->pixels);
    s->con = qemu_graphic_console_create(dev, 0, &ws2812_panel_ops, s);
    qemu_console_resize(s->con, s->pixels * s->scale,
                        s->strips * s->scale + ws2812_panel_band(s));
}

static void ws2812_panel_unrealize(DeviceState *dev)
{
    WS2812PanelState *s = WS2812_PANEL(dev);

    if (s->dump_file) {
        ws2812_panel_flush_switches(s, -1);
        fclose(s->dump_file);
    }
    g_free(s->rgb);
}

static const Property ws2812_panel_properties[] = {
    DEFINE_PROP_UINT32("strips", WS2812PanelState, strips, 1),
    DEFINE_PROP_UINT32("pixels", WS2812PanelState, pixels, 1),
    DEFINE_PROP_UINT32("switches", WS2812PanelState, switches, 0),
    DEFINE_PROP_UINT32("scale", WS2812PanelState, scale, 2),
    DEFINE_PROP_STRING("dump", WS2812PanelState, dump),
};

static void ws2812_panel_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = ws2812_panel_realize;
    dc->unrealize = ws2812_panel_unrealize;
    dc->user_creatable = false;
    device_class_set_props(dc, ws2812_panel_properties);
}

static const TypeInfo ws2812_panel_types[] = {
    {
        .name = TYPE_WS2812_PANEL,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(WS2812PanelState),
        .class_init = ws2812_panel_class_init,
    },
};

DEFINE_TYPES(ws2812_panel_types)
