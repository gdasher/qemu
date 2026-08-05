/*
 * Simulation bridge: pin-level co-simulation with an external physical model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_SIM_BRIDGE_H
#define HW_PIC16_SIM_BRIDGE_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_PIC16_SIM_BRIDGE "pic16-sim-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(PIC16SimBridge, PIC16_SIM_BRIDGE)

#define PIC16_SIM_BRIDGE_MAX_LINES 64

/* What the model asked to be told about, per line. */
typedef enum {
    PIC16_WATCH_NONE = 0,
    PIC16_WATCH_RISING = 1,
    PIC16_WATCH_FALLING = 2,
    PIC16_WATCH_BOTH = 3,
} PIC16WatchMode;

struct PIC16SimBridge {
    SysBusDevice parent_obj;

    CharFrontend chr;
    QEMUTimer *deadline;

    char *names[PIC16_SIM_BRIDGE_MAX_LINES];
    unsigned n_lines;

    uint8_t level[PIC16_SIM_BRIDGE_MAX_LINES];   /* as the chip drives it */
    uint8_t watch[PIC16_SIM_BRIDGE_MAX_LINES];
    bool driven[PIC16_SIM_BRIDGE_MAX_LINES];     /* model claimed this line */

    qemu_irq out[PIC16_SIM_BRIDGE_MAX_LINES];    /* model -> chip */

    bool ready;         /* handshake completed */
    bool failed;        /* protocol error or disconnect; stop talking */

    GString *rx;
};

/**
 * pic16_sim_bridge_add_line:
 *
 * Registers a line under its hardware name -- "soc.RA4", "expander.GP0" -- and
 * returns its index. Call before realize. The board connects the chip's output
 * for that pin to the bridge's matching GPIO input, and the bridge's matching
 * GPIO output to the chip's input.
 */
int pic16_sim_bridge_add_line(PIC16SimBridge *b, const char *name);

#endif /* HW_PIC16_SIM_BRIDGE_H */
