/*
 * PIC18 Windowed Watchdog Timer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC18_WWDT_H
#define HW_PIC18_WWDT_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "target/pic18/cpu.h"
#include "qom/object.h"

#define TYPE_PIC18_WWDT "pic18-wwdt"
OBJECT_DECLARE_SIMPLE_TYPE(PIC18WwdtState, PIC18_WWDT)

/* Named GPIO the CPU pulses when it executes CLRWDT. */
#define PIC18_WWDT_CLEAR_GPIO "clrwdt"

struct PIC18WwdtState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    ptimer_state *timer;

    uint8_t con0;
    uint8_t con1;

    /* Set once the watchdog has expired, so PCON0 can report why. */
    bool expired;

    PIC18CPU *cpu;
};

#endif /* HW_PIC18_WWDT_H */
