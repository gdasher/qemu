/*
 * PIC32 watchdog timer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32_WDT_H
#define HW_PIC32_PIC32_WDT_H

#include "hw/core/ptimer.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PIC32_WDT "pic32-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(PIC32WdtState, PIC32_WDT)

#define PIC32_WDT_SIZE 0x10

/* Fires when the watchdog runs out, so the board can record why it reset. */
#define PIC32_WDT_TIMEOUT_GPIO "timeout"

struct PIC32WdtState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    ptimer_state *timer;

    /*
     * What the configuration words leave behind: whether the watchdog is
     * running before software touches it, and the divider it counts the
     * low-power oscillator down by.
     */
    bool enabled;
    uint32_t rundiv;

    uint32_t con;

    qemu_irq timeout;
};

#endif /* HW_PIC32_PIC32_WDT_H */
