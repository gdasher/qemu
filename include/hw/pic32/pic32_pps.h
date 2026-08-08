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
};

#endif /* HW_PIC32_PIC32_PPS_H */
