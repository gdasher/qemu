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

#define TYPE_PIC16F1_SOC "pic16f1-soc"
#define TYPE_PIC16F17546_SOC "pic16f17546-soc"
OBJECT_DECLARE_TYPE(PIC16F1SocState, PIC16F1SocClass, PIC16F1_SOC)

#define PIC16_NUM_PIR 8

/*
 * Interrupt lines are numbered by their position in the PIR registers, so a
 * peripheral asks for the line matching the flag it owns.
 */
#define PIC16_IRQ(reg, bit) ((reg) * 8 + (bit))
#define PIC16_NUM_IRQ (PIC16_NUM_PIR * 8)

/* Named GPIO array carrying those lines into the SoC. */
#define PIC16_IRQ_GPIO "pir"

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

    uint8_t pir[PIC16_NUM_PIR];
    uint8_t pie[PIC16_NUM_PIR];
    uint8_t pcon0;
    uint8_t pcon1;

    qemu_irq cpu_irq;
};

#endif /* HW_PIC16_PIC16F1_SOC_H */
