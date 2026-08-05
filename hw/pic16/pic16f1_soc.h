/*
 * PIC16F1xxxx SoC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_PIC16F1_SOC_H
#define HW_PIC16_PIC16F1_SOC_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "target/pic16/cpu.h"
#include "qom/object.h"
#include "pic16_port.h"
#include "pic16_eusart.h"
#include "pic16_mssp.h"
#include "pic16_tmr1.h"
#include "pic16_wwdt.h"

#define TYPE_PIC16F1_SOC "pic16f1-soc"
#define TYPE_PIC16F17546_SOC "pic16f17546-soc"
OBJECT_DECLARE_TYPE(PIC16F1SocState, PIC16F1SocClass, PIC16F1_SOC)

#define PIC16_NUM_PIR 8

/* Peripheral pin select: output selects in bank 59, input selects in bank 60. */
#define PIC16_PPS_OUT_ADDR 0x1D8C
#define PIC16_PPS_OUT_SIZE 0x24
#define PIC16_PPS_IN_ADDR  0x1E0C
#define PIC16_PPS_IN_SIZE  0x54

/*
 * Interrupt lines are numbered by their position in the PIR registers, so a
 * peripheral asks for the line matching the flag it owns.
 */
#define PIC16_IRQ(reg, bit) ((reg) * 8 + (bit))
#define PIC16_NUM_IRQ (PIC16_NUM_PIR * 8)

/*
 * Two named GPIO arrays carry those lines in. A latching line sets its PIR
 * flag on a rising edge and only software clears it again; a level line is
 * held by its peripheral and software cannot clear it, which is how the
 * read-only flags behave -- TXxIF and RCxIF are dismissed by touching the
 * data register, and IOCIF simply reflects whether any IOCxF bit is set.
 */
#define PIC16_IRQ_GPIO "pir"
#define PIC16_IRQ_LEVEL_GPIO "pir-level"

/* Line assignments, read out of the compiled firmware's interrupt handler. */
#define PIC16_IRQ_IOC  PIC16_IRQ(0, 4)
#define PIC16_IRQ_TMR1 PIC16_IRQ(1, 6)
#define PIC16_IRQ_TX1  PIC16_IRQ(4, 6)
#define PIC16_IRQ_RC1  PIC16_IRQ(4, 7)

struct PIC16F1SocClass {
    SysBusDeviceClass parent_class;

    const char *cpu_type;
    unsigned flash_words;   /* implemented program memory */
    unsigned gpr_banks;     /* banks with an 80-byte GPR block */
    uint64_t fosc_hz;
};

struct PIC16F1SocState {
    SysBusDevice parent_obj;

    PIC16CPU cpu;
    Clock *fosc;

    MemoryRegion flash;
    MemoryRegion config;
    MemoryRegion common;
    MemoryRegion gpr[PIC16_NUM_BANKS];
    MemoryRegion intc;
    MemoryRegion pcon;
    MemoryRegion core_sfr;
    MemoryRegion pps_out;
    MemoryRegion pps_in;

    PIC16PortState port;
    PIC16EusartState eusart1;
    PIC16MsspState mssp1;
    PIC16Tmr1State tmr1;
    PIC16WwdtState wwdt;

    uint8_t pir_latch[PIC16_NUM_PIR];
    uint8_t pir_level[PIC16_NUM_PIR];
    uint8_t pie[PIC16_NUM_PIR];
    uint8_t pps_out_regs[PIC16_PPS_OUT_SIZE];
    uint8_t pps_in_regs[PIC16_PPS_IN_SIZE];
    uint8_t pcon1;
    bool por;
    bool bor;

    qemu_irq cpu_irq;
};

#endif /* HW_PIC16_PIC16F1_SOC_H */
