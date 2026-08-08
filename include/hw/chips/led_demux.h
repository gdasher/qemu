/*
 * One-of-N demultiplexer on a signal line
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHIPS_LED_DEMUX_H
#define HW_CHIPS_LED_DEMUX_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_LED_DEMUX "led-demux"
OBJECT_DECLARE_SIMPLE_TYPE(LedDemuxState, LED_DEMUX)

#define LED_DEMUX_MAX_OUTPUTS 16
#define LED_DEMUX_MAX_SELECTS 4

/* The data line, the address lines, and the active-low output enable. */
#define LED_DEMUX_IN_GPIO "in"
#define LED_DEMUX_SELECT_GPIO "select"
#define LED_DEMUX_ENABLE_GPIO "enable-n"
#define LED_DEMUX_OUT_GPIO "out"

struct LedDemuxState {
    SysBusDevice parent_obj;

    uint32_t outputs;
    uint32_t selects;   /* how many address lines that needs */

    bool level;         /* what the data line carries */
    bool disabled;      /* what the enable line says, in its own sense */
    uint32_t address;

    qemu_irq out[LED_DEMUX_MAX_OUTPUTS];
};

#endif /* HW_CHIPS_LED_DEMUX_H */
