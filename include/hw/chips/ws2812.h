/*
 * WS2812 addressable LED strip
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_WS2812_H
#define HW_PIC16_WS2812_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_WS2812 "ws2812"
OBJECT_DECLARE_SIMPLE_TYPE(WS2812State, WS2812)

/* Named GPIO array carrying the strip's data line. */
#define WS2812_IN_GPIO "din"

#define WS2812_MAX_PIXELS 1024
#define WS2812_BITS_PER_PIXEL 24

/*
 * Where a latched frame goes. The strip decodes the wire; what a frame means
 * to whoever is watching -- a line on a protocol, a picture on a screen, a
 * file on disk -- belongs to the board, so the board says where to send it.
 * Pixels are RGB, one per uint32, and the array does not outlive the call.
 */
typedef void (*WS2812FrameFn)(void *opaque, WS2812State *s,
                              const uint32_t *rgb, unsigned pixels);

struct WS2812State {
    SysBusDevice parent_obj;

    uint32_t pixels;    /* how long the strip is */
    /*
     * The order the controller is expected to send the three bytes in. The
     * part's own is green, red, blue; parts that call themselves WS2812 and
     * take red first are common enough, and a firmware written for one of
     * those looks like it has red and green swapped when decoded as the data
     * sheet says.
     */
    char *order;
    char *name;         /* what the board calls it, e.g. "led.RB7" */

    WS2812FrameFn frame;
    void *frame_opaque;

    /*
     * The frame being shifted in. Highs are buffered rather than decoded on
     * the spot: the threshold comes from the bit period, and the period is
     * not known until a bit has been through both of its halves.
     */
    uint32_t high_ns[WS2812_MAX_PIXELS * WS2812_BITS_PER_PIXEL];
    uint32_t n_bits;
    uint32_t period_ns;  /* of the first complete bit in this frame */
    bool overrun;        /* more bits arrived than the strip has */

    /*
     * What told a one from a zero the last time a frame carried both, kept
     * because a frame that carries only one of them cannot say on its own.
     * It describes the controller's timing rather than the board's state, so
     * it outlives a reset.
     */
    uint32_t learned_ns;

    bool rgb_order;     /* what "order" resolved to */

    bool level;
    int64_t edge_ns;     /* when the line last changed */
    QEMUTimer *quiet;    /* fires when the guest stops shifting */
};

/* Says where latched frames go. Call before realize. */
void ws2812_set_frame_sink(WS2812State *s, WS2812FrameFn fn, void *opaque);

#endif /* HW_PIC16_WS2812_H */
