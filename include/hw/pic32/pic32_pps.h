/*
 * PIC32 peripheral pin select
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32_PPS_H
#define HW_PIC32_PIC32_PPS_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PIC32_PPS "pic32-pps"
OBJECT_DECLARE_SIMPLE_TYPE(PIC32PpsState, PIC32_PPS)

/* Input selects at 0x000, output selects at 0x200, four bytes apart. */
#define PIC32_PPS_SIZE 0x400
#define PIC32_PPS_REGS (PIC32_PPS_SIZE / 4)

struct PIC32PpsState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[PIC32_PPS_REGS];

    /*
     * CFGCON.IOLOCK as the SoC last relayed it. Not migrated: it is derived
     * state, and the CRU republishes it after a load.
     */
    bool locked;

    void (*out_notify)(void *opaque, unsigned reg, unsigned sel);
    void *out_opaque;
};

/* While IOLOCK is set, hardware discards writes to every select register. */
void pic32_pps_set_locked(PIC32PpsState *s, bool locked);

/*
 * Output selects live at 0x200, four bytes apart, so RPA14R -- the register
 * that says what drives the LED data pin -- is PIC32_PPS_OUT_RPA14. A board
 * that cares which peripheral reaches a pin asks to be told when one of these
 * changes; nothing else about peripheral pin select is modelled, because a
 * pin the firmware never remaps is a pin the board can wire directly.
 */
#define PIC32_PPS_OUT(off) (((off) - 0x1400) / 4)
#define PIC32_PPS_OUT_RPA14 PIC32_PPS_OUT(0x1638)

/* RPnR values, from the output pin selection table. */
#define PIC32_PPS_OUT_SDO4 0x0F

void pic32_pps_set_out_notifier(PIC32PpsState *s,
                                void (*fn)(void *opaque, unsigned reg,
                                           unsigned sel),
                                void *opaque);
unsigned pic32_pps_out_get(PIC32PpsState *s, unsigned reg);

#endif /* HW_PIC32_PIC32_PPS_H */
