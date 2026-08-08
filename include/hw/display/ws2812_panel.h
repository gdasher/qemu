/*
 * A view of what a set of WS2812 strips is showing
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_WS2812_PANEL_H
#define HW_DISPLAY_WS2812_PANEL_H

#include "hw/core/sysbus.h"
#include "hw/chips/ws2812.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_WS2812_PANEL "ws2812-panel"
OBJECT_DECLARE_SIMPLE_TYPE(WS2812PanelState, WS2812_PANEL)

#define WS2812_PANEL_MAX_STRIPS 16

struct WS2812PanelState {
    SysBusDevice parent_obj;

    uint32_t strips;
    uint32_t pixels;
    uint32_t scale;     /* screen pixels per LED, each way */
    char *dump;         /* where to write frames, or NULL */

    QemuConsole *con;
    FILE *dump_file;
    bool dirty;

    /* One strip's worth of colours per row, most recently latched. */
    uint32_t *rgb;

    /* The strips being watched, in the order the board added them. */
    WS2812State *strip[WS2812_PANEL_MAX_STRIPS];
    unsigned n_strips;
};

/*
 * Watches one more strip. The order they are added in is the order they appear
 * on screen, so the board adds them in whatever order the product numbers its
 * strings.
 */
void ws2812_panel_add(WS2812PanelState *s, WS2812State *strip);

#endif /* HW_DISPLAY_WS2812_PANEL_H */
