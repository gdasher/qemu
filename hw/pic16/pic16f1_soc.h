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
#include "pic16_tmr0.h"
#include "pic16_nco.h"
#include "pic16_tmr1.h"
#include "pic16_wwdt.h"

#define TYPE_PIC16F1_SOC "pic16f1-soc"
#define TYPE_PIC16F17546_SOC "pic16f17546-soc"
#define TYPE_PIC16F15354_SOC "pic16f15354-soc"
#define TYPE_PIC16F15355_SOC "pic16f15355-soc"
OBJECT_DECLARE_TYPE(PIC16F1SocState, PIC16F1SocClass, PIC16F1_SOC)

#define PIC16_NUM_PIR 8

#define PIC16_MAX_PPS_OUT_SIZE 0x24
#define PIC16_MAX_PPS_IN_SIZE  0x54

/*
 * Interrupt lines are numbered by their position in the PIR registers, so a
 * peripheral asks for the line matching the flag it owns.
 */
#define PIC16_IRQ(reg, bit) ((reg) * 8 + (bit))
#define PIC16_NUM_IRQ (PIC16_NUM_PIR * 8)

/*
 * Two named GPIO arrays carry those lines in. A latching line sets its PIR
 * flag on a rising edge and only software clears it again; a level line is
 * held by its peripheral and software cannot clear it.
 */
#define PIC16_IRQ_GPIO "pir"
#define PIC16_IRQ_LEVEL_GPIO "pir-level"

struct PIC16F1SocClass {
    SysBusDeviceClass parent_class;

    const char *cpu_type;
    unsigned flash_words;   /* implemented program memory */
    unsigned gpr_banks;     /* banks with a GPR block */
    unsigned gpr_last_bank_size; /* bytes in the last of them; 0 for all 80 */
    uint64_t fosc_hz;
    uint8_t port_layout;

    hwaddr pir_addr;
    hwaddr pcon_addr;
    hwaddr core_sfr_addr;
    hwaddr pps_out_addr;
    unsigned pps_out_size;
    hwaddr pps_in_addr;
    unsigned pps_in_size;
    hwaddr port_data_addr;
    hwaddr port_pad_addr;
    hwaddr eusart1_addr;
    /*
     * Serial 0 goes to EUSART1 when there is one, unless the part's boards
     * use MSSP1 as an SPI slave link and want the chardev there instead.
     */
    bool serial0_mssp1;
    hwaddr mssp1_addr;
    hwaddr mssp2_addr;
    hwaddr tmr0_addr;
    hwaddr tmr1_addr;
    hwaddr nco1_addr;
    hwaddr wwdt_addr;
    hwaddr osc_addr;
    hwaddr pmd_addr;

    int irq_ioc;
    int irq_tmr0;
    int irq_ssp1;
    int irq_ssp2;
    int irq_tmr1;
    int irq_tx1;
    int irq_rc1;
    int irq_nco1;
};

struct PIC16F1SocState {
    SysBusDevice parent_obj;

    PIC16CPU cpu;
    Clock *fosc;
    int32_t osc_ppm;
    uint64_t base_fosc_hz;

    MemoryRegion flash;
    MemoryRegion config;
    MemoryRegion common;
    MemoryRegion gpr[PIC16_NUM_BANKS];
    MemoryRegion intc;
    MemoryRegion pcon;
    MemoryRegion core_sfr;
    MemoryRegion pps_out;
    MemoryRegion pps_in;
    MemoryRegion osc;
    MemoryRegion pmd;

    PIC16PortState port;
    PIC16EusartState eusart1;
    PIC16MsspState mssp1;
    PIC16MsspState mssp2;
    PIC16Tmr0State tmr0;
    PIC16NcoState nco1;
    PIC16Tmr1State tmr1;
    PIC16WwdtState wwdt;

    uint8_t pir_latch[PIC16_NUM_PIR];
    uint8_t pir_level[PIC16_NUM_PIR];
    uint8_t pie[PIC16_NUM_PIR];
    uint8_t pps_out_regs[PIC16_MAX_PPS_OUT_SIZE];
    uint8_t pps_in_regs[PIC16_MAX_PPS_IN_SIZE];
    uint8_t osc_regs[16];
    uint8_t pmd_regs[8];
    uint8_t pcon1;
    bool por;
    bool bor;

    qemu_irq cpu_irq;
};

#endif /* HW_PIC16_PIC16F1_SOC_H */
