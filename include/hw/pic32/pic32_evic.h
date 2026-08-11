/*
 * PIC32 interrupt controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32_EVIC_H
#define HW_PIC32_PIC32_EVIC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "target/mips/cpu.h"

#define TYPE_PIC32_EVIC "pic32-evic"
OBJECT_DECLARE_SIMPLE_TYPE(PIC32EvicState, PIC32_EVIC)

/*
 * Two regions: the control, flag, enable and priority registers, which have
 * the atomic aliases, and the block of per-source vector offsets, which are
 * four bytes apart and have none.
 */
#define PIC32_EVIC_CTRL_SIZE 0x540
#define PIC32_EVIC_OFF_BASE  0x540
#define PIC32_EVIC_OFF_SIZE  0x400

#define PIC32_EVIC_SOURCES 256
#define PIC32_EVIC_IFS_REGS (PIC32_EVIC_SOURCES / 32)
#define PIC32_EVIC_IPC_REGS (PIC32_EVIC_SOURCES / 4)

/*
 * Two named GPIO arrays, one per source in each. A source on "irq" latches:
 * the peripheral pulses it and the flag stays set until software clears it,
 * which is how a timer's period match behaves. A source on "irq-level" is
 * held: the flag reads as set for as long as the peripheral drives the line,
 * and software cannot clear it, which is how the data sheet's "persistent"
 * sources behave -- a UART whose receiver still holds a character will simply
 * set its flag again.
 */
#define PIC32_EVIC_IRQ_GPIO "irq"
#define PIC32_EVIC_IRQ_LEVEL_GPIO "irq-level"

/* Sources this machine has something to connect to. */
enum {
    PIC32_IRQ_CORE_TIMER = 0,
    PIC32_IRQ_CORE_SW0 = 1,
    PIC32_IRQ_CORE_SW1 = 2,
    PIC32_IRQ_TIMER1 = 4,
    PIC32_IRQ_TIMER2 = 9,
    PIC32_IRQ_TIMER3 = 14,
    PIC32_IRQ_SPI1_FAULT = 35,
    PIC32_IRQ_UART1_FAULT = 38,
    PIC32_IRQ_PMP = 51,
    PIC32_IRQ_UART2_FAULT = 56,
    PIC32_IRQ_DMA0 = 72,
    PIC32_IRQ_DMA4 = 182,
    PIC32_IRQ_SPI3_FAULT = 218,
    PIC32_IRQ_SPI4_FAULT = 221,
};

struct PIC32DmacState;

struct PIC32EvicState {
    SysBusDevice parent_obj;

    MemoryRegion ctrl;
    MemoryRegion off;

    MIPSCPU *cpu;

    /* Set by the SoC: every source is echoed here so a channel can start. */
    struct PIC32DmacState *dmac;

    uint32_t intcon;
    uint32_t priss;
    uint32_t iptmr;
    uint32_t ifs[PIC32_EVIC_IFS_REGS];
    uint32_t iec[PIC32_EVIC_IFS_REGS];
    uint32_t ipc[PIC32_EVIC_IPC_REGS];
    uint32_t offset[PIC32_EVIC_SOURCES];

    /* Which held lines are currently driven, alongside the latched flags. */
    uint32_t level[PIC32_EVIC_IFS_REGS];

    /* What was last requested of the core, so a no-op change costs nothing. */
    uint32_t requested_ripl;
    uint32_t requested_offset;
    uint32_t requested_src;
};

#endif /* HW_PIC32_PIC32_EVIC_H */
