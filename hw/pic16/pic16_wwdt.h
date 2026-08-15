/*
 * PIC16 Windowed Watchdog Timer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_WWDT_H
#define HW_PIC16_WWDT_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "target/pic16/cpu.h"
#include "qom/object.h"

#define TYPE_PIC16_WWDT "pic16-wwdt"
OBJECT_DECLARE_SIMPLE_TYPE(PIC16WwdtState, PIC16_WWDT)

/* Named GPIO the CPU pulses when it executes CLRWDT. */
#define PIC16_WWDT_CLEAR_GPIO "clrwdt"

struct PIC16WwdtState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    ptimer_state *timer;

    uint8_t con0;
    uint8_t con1;

    /* Set once the watchdog has expired, so PCON0 can report why. */
    bool expired;

    PIC16CPU *cpu;
};

#endif /* HW_PIC16_WWDT_H */
