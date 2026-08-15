/*
 * PIC16 Numerically Controlled Oscillator (NCO)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_NCO_H
#define HW_PIC16_NCO_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qom/object.h"

#define TYPE_PIC16_NCO "pic16-nco"
OBJECT_DECLARE_SIMPLE_TYPE(PIC16NcoState, PIC16_NCO)

enum {
    REG_NCO_ACCL,
    REG_NCO_ACCH,
    REG_NCO_ACCU,
    REG_NCO_INCL,
    REG_NCO_INCH,
    REG_NCO_INCU,
    REG_NCO_CON,
    REG_NCO_CLK,
    PIC16_NCO_NREGS,
};

struct PIC16NcoState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    Clock *fosc;

    uint32_t acc;
    uint32_t inc;

    uint8_t acc_buf[3];
    uint8_t inc_buf[3];
    uint8_t con;
    uint8_t clk;

    qemu_irq irq;
    qemu_irq out;
};

#endif /* HW_PIC16_NCO_H */
