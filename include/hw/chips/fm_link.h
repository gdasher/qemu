/*
 * Co-simulation link to the FM transmitter's own microcontroller
 *
 * Stands where hw/chips/fm_transmitter.c stands -- an SPI slave on the
 * controller's bus, selected by a port pin -- but models nothing. Every byte
 * goes to a second QEMU running the transmitter's real firmware, and the
 * answer comes back from it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHIPS_FM_LINK_H
#define HW_CHIPS_FM_LINK_H

#include "chardev/char-fe.h"
#include "hw/ssi/ssi.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_FM_LINK "fm-link"
OBJECT_DECLARE_SIMPLE_TYPE(FMLinkState, FM_LINK)

/* The protocol version both ends announce at reset.
   Version 2 added the queue level lines: the master polls with
   "P <t_ns>" on its sync heartbeat and the slave answers "L <val>",
   val bit 0 = LVL0 (the slave's RC0), bit 1 = LVL1 (RC7). */
#define FM_LINK_VERSION 2

/* The same name the fm-transmitter model uses for its level lines, so the
   board wires either the model or the link identically. */
#define FM_LINK_LVL_GPIO "lvl"

struct FMLinkState {
    SSIPeripheral parent_obj;

    CharFrontend chr;
    GString *rx;                /* what has arrived and not yet been used */

    char *dump_path;
    FILE *dump_file;
    uint64_t sync_ns;           /* how often to tell the slave the time */

    QEMUTimer *sync;
    bool started;               /* the handshake has been done */
    bool failed;                /* the slave is gone; stop talking to it */
    bool selected;
    uint8_t last_rx;

    /* The queue level lines, polled over the link (see FM_LINK_VERSION). */
    uint8_t lvl;
    qemu_irq lvl_out[2];
};

#endif /* HW_CHIPS_FM_LINK_H */
