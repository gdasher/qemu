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
#define WS2812_PANEL_MAX_SWITCHES 8

/*
 * How big a switch is drawn, in LED pixels. A relay is not a pixel and should
 * not be mistaken for one, so it gets a block that is obviously not a member
 * of the rows above it.
 */
#define WS2812_PANEL_SWITCH_PIXELS 8

struct WS2812PanelState {
    SysBusDevice parent_obj;

    uint32_t strips;
    uint32_t pixels;
    uint32_t switches;  /* on/off outputs shown under the strips */
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

    uint8_t switch_state;
    /*
     * The switch line not yet written. An expander drives its pins one at a
     * time, so one register write walks through several states in no time at
     * all; holding the line until the clock moves on writes where it settled
     * rather than every step on the way.
     */
    bool switch_pending;
    int64_t switch_time;
};

/*
 * Watches one more strip. The order they are added in is the order they appear
 * on screen, so the board adds them in whatever order the product numbers its
 * strings.
 */
void ws2812_panel_add(WS2812PanelState *s, WS2812State *strip);

/*
 * Sets one of the on/off outputs. The board wires these to whatever drives its
 * relays, which is not a strip and does not latch a frame: the picture changes
 * when the line does.
 */
void ws2812_panel_set_switch(WS2812PanelState *s, unsigned index, bool on);

#endif /* HW_DISPLAY_WS2812_PANEL_H */
