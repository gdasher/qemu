/*
 * PIC16 Timer0 (8-bit / 16-bit Timer with Period Match)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_TMR0_H
#define HW_PIC16_TMR0_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"

#define TYPE_PIC16_TMR0 "pic16-tmr0"
OBJECT_DECLARE_SIMPLE_TYPE(PIC16Tmr0State, PIC16_TMR0)

enum {
    REG_TMR0L,
    REG_TMR0H,
    REG_T0CON0,
    REG_T0CON1,
    PIC16_TMR0_NREGS,
};

struct PIC16Tmr0State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    ptimer_state *timer;
    Clock *fosc;

    uint8_t tmr0l;
    uint8_t tmr0h;
    uint8_t con0;
    uint8_t con1;

    /* High byte buffer for 16-bit mode */
    uint8_t tmr0h_buf;

    qemu_irq irq;
};

#endif /* HW_PIC16_TMR0_H */
