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
#include "pic16_sim_bridge.h"

#define TYPE_WS2812 "ws2812"
OBJECT_DECLARE_SIMPLE_TYPE(WS2812State, WS2812)

/* Named GPIO array carrying the strip's data line. */
#define WS2812_IN_GPIO "din"

#define WS2812_MAX_PIXELS 1024
#define WS2812_BITS_PER_PIXEL 24

struct WS2812State {
    SysBusDevice parent_obj;

    uint32_t pixels;    /* how long the strip is */
    char *name;         /* what the model calls it, e.g. "led.RB7" */
    PIC16SimBridge *bridge;

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

    bool level;
    int64_t edge_ns;     /* when the line last changed */
    QEMUTimer *quiet;    /* fires when the guest stops shifting */
};

#endif /* HW_PIC16_WS2812_H */
