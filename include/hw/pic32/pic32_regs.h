/*
 * PIC32 register blocks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32_REGS_H
#define HW_PIC32_PIC32_REGS_H

#include "system/memory.h"

/*
 * Most PIC32 registers appear four times: the register itself, then aliases at
 * +4, +8 and +0xC that clear, set and invert the bits written as one. Firmware
 * uses them constantly -- Harmony writes IEC0SET rather than a read-modify-
 * write of IEC0 -- so every device would otherwise decode the same three
 * offsets again, and every device would be a chance to get one of them wrong.
 *
 * A device with aliases lays its registers out 16 bytes apart and asks for
 * PIC32_REGS_ALIASED; one whose registers are four bytes apart and have no
 * aliases (peripheral pin select, the interrupt controller's vector offsets)
 * asks for PIC32_REGS_PLAIN. A device that is both -- the EVIC -- gives each
 * part its own region, which is how the data sheet describes it too.
 */
#define PIC32_REGS_ALIASED 0x10
#define PIC32_REGS_PLAIN   0x04

typedef struct PIC32RegsOps {
    /* Both are given the offset of the plain register, never of an alias. */
    uint32_t (*read)(void *opaque, hwaddr offset);
    void (*write)(void *opaque, hwaddr offset, uint32_t value);
} PIC32RegsOps;

/*
 * Builds the MMIO region for one block. Sub-word accesses are read-modify-
 * write, because firmware does make them -- feeding the watchdog is a 16-bit
 * store to the top half of WDTCON.
 */
void pic32_regs_init_io(MemoryRegion *mr, Object *owner,
                        const PIC32RegsOps *ops, void *opaque,
                        const char *name, uint64_t size, unsigned stride);

/*
 * Ties a block built by pic32_regs_init_io() to a Peripheral Module Disable
 * bit. While *disabled is true the block answers the way the clock-stopped
 * hardware does: reads return zero, writes change nothing, and the first
 * access is logged as a guest error, because on the real part it means the
 * firmware is talking to a module it turned off. The flag stays the
 * caller's; whoever owns the PMD registers keeps it current.
 */
void pic32_regs_set_pmd_gate(MemoryRegion *mr, const bool *disabled);

#endif /* HW_PIC32_PIC32_REGS_H */
