/*
 * One-of-N demultiplexer on a signal line
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHIPS_LED_DEMUX_H
#define HW_CHIPS_LED_DEMUX_H

#include "hw/core/sysbus.h"
#include "hw/chips/vcd_trace.h"
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

    /* Where the part's own lines are traced, and their handles there. */
    VcdTrace *trace;
    int sig_in;
    int sig_select;
    int sig_enable;
    int sig_out[LED_DEMUX_MAX_OUTPUTS];
};

/*
 * Traces the data line, the address, the enable and every output. Call it
 * after realize, since the part has to know how many outputs it has before it
 * can name them, and before anything is traced, since a VCD names its signals
 * up front.
 */
void led_demux_set_trace(LedDemuxState *s, VcdTrace *t);

#endif /* HW_CHIPS_LED_DEMUX_H */
