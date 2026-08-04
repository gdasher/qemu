/*
 * PIC16 Timer1
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_TMR1_H
#define HW_PIC16_TMR1_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "hw/core/clock.h"
#include "qom/object.h"

#define TYPE_PIC16_TMR1 "pic16-tmr1"
OBJECT_DECLARE_SIMPLE_TYPE(PIC16Tmr1State, PIC16_TMR1)

struct PIC16Tmr1State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    ptimer_state *timer;
    Clock *fosc;

    uint8_t con;
    uint8_t gcon;
    uint8_t gate;
    uint8_t clk;

    /* Latched high byte, as the hardware buffers 16-bit reads and writes. */
    uint8_t high;

    qemu_irq irq;
};

#endif /* HW_PIC16_TMR1_H */
