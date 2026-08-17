/*
 * The slave end of the FM co-simulation link
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_COSIM_LINK_H
#define HW_PIC16_COSIM_LINK_H

#include "chardev/char-fe.h"
#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "pic16_mssp.h"

#define TYPE_PIC16_COSIM_LINK "pic16-cosim-link"
OBJECT_DECLARE_SIMPLE_TYPE(PIC16CosimLink, PIC16_COSIM_LINK)

struct PIC16CosimLink {
    SysBusDevice parent_obj;

    CharFrontend chr;
    GString *rx;

    PIC16MsspState *mssp;       /* where a byte off the link is clocked in */

    QEMUTimer *gate;            /* fires when the guest reaches gate_ns */
    int64_t gate_ns;            /* the latest moment the master has allowed */
    bool started;
    bool failed;
    bool done;                  /* the master said goodbye; run free */

    /* A byte whose moment has not come. The master waits for its reply, so
     * there is never more than one. */
    bool pending;
    int64_t pending_ns;
    uint8_t pending_byte;

    uint64_t bytes;
    uint64_t late;              /* bytes stamped before the guest's own clock */
};

void pic16_cosim_link_set_mssp(PIC16CosimLink *l, PIC16MsspState *mssp);

#endif /* HW_PIC16_COSIM_LINK_H */
