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

static void ws2812_panel_write_dump(WS2812PanelState *s, unsigned index,
                                    const uint32_t *rgb, unsigned pixels)
{
    unsigned i = 0;

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
    qemu_console_resize(s->con, s->pixels * s->scale, s->strips * s->scale);
}

static void ws2812_panel_unrealize(DeviceState *dev)
{
    WS2812PanelState *s = WS2812_PANEL(dev);

    if (s->dump_file) {
        fclose(s->dump_file);
    }
    g_free(s->rgb);
}

static const Property ws2812_panel_properties[] = {
    DEFINE_PROP_UINT32("strips", WS2812PanelState, strips, 1),
    DEFINE_PROP_UINT32("pixels", WS2812PanelState, pixels, 1),
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
