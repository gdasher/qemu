/*
 * PIC32 timers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32_TIMER_H
#define HW_PIC32_PIC32_TIMER_H

#include "hw/core/clock.h"
#include "hw/core/ptimer.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PIC32_TIMER "pic32-timer"
OBJECT_DECLARE_SIMPLE_TYPE(PIC32TimerState, PIC32_TIMER)

#define PIC32_TIMER_SIZE 0x30

/* One interrupt, raised when the count rolls over its period. */
#define PIC32_TIMER_IRQ_GPIO "irq"

struct PIC32TimerState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    Clock *pbclk;
    ptimer_state *timer;

    /*
     * Timer1 is the odd one out: the family calls it a type A timer, and its
     * prescaler is two bits selecting 1, 8, 64 or 256 where the others have
     * three selecting every power of two up to 256.
     */
    bool type_a;

    uint32_t con;
    uint32_t pr;

    qemu_irq irq;
};

#endif /* HW_PIC32_PIC32_TIMER_H */
