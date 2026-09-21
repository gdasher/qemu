/*
 * PIC18F-Q43 SoC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC18_PIC18Q43_SOC_H
#define HW_PIC18_PIC18Q43_SOC_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"
#include "target/pic18/cpu.h"
#include "qom/object.h"
#include "pic18_port.h"
#include "pic18_spi.h"
#include "pic18_tmr0.h"
#include "pic18_wwdt.h"

#define TYPE_PIC18Q43_SOC "pic18q43-soc"
#define TYPE_PIC18F27Q43_SOC "pic18f27q43-soc"
OBJECT_DECLARE_TYPE(PIC18Q43SocState, PIC18Q43SocClass, PIC18Q43_SOC)

#define PIC18_NUM_PIR 16

/* PPSLOCK and RA0PPS..RC7PPS; INT0PPS..SPI2SSPPS and the rest of the block. */
#define PIC18_PPS_OUT_SIZE 0x19
#define PIC18_PPS_IN_SIZE  0x42
#define PIC18_PPS_OUT_LINES 24

#define PIC18_OSC_NREGS 7
#define PIC18_PMD_NREGS 9
#define PIC18_IVT_NREGS 8       /* IVTLOCK 0x459 .. IVTBASEU 0x45F */

typedef struct PIC18DriftProfile {
    bool    enabled;
    int32_t thermal_max_ppm;   /* e.g. +3500 ppm */
    int32_t thermal_tau_ms;    /* e.g. 2000 ms time constant */
    int32_t linear_ppm_per_s;  /* e.g. +50 ppm/s */
    int32_t ripple_amp_ppm;    /* e.g. 100 ppm */
    double  ripple_freq_hz;    /* e.g. 2.0 Hz */
    int32_t jitter_sigma_ppm;  /* e.g. 10 ppm */
    double  accumulated_jitter;
} PIC18DriftProfile;

/*
 * Interrupt lines are numbered by their position in the PIR registers, so a
 * peripheral asks for the line matching the flag it owns.
 */
#define PIC18_IRQ(reg, bit) ((reg) * 8 + (bit))
#define PIC18_NUM_IRQ (PIC18_NUM_PIR * 8)

/*
 * Two named GPIO arrays carry those lines in. A latching line sets its PIR
 * flag on a rising edge and only software clears it again; a level line is
 * held by its peripheral and software cannot clear it.
 */
#define PIC18_IRQ_GPIO "pir"
#define PIC18_IRQ_LEVEL_GPIO "pir-level"

/* The SPI modules' SS(out) signals, routed to pins by RxyPPS. */
#define PIC18_SOC_SPI_SS_GPIO "spi-ss"

struct PIC18Q43SocClass {
    SysBusDeviceClass parent_class;

    const char *cpu_type;
    unsigned flash_size;    /* bytes of program flash */
    unsigned gpr_size;      /* bytes of data RAM above the SFRs */
    uint64_t fosc_hz;       /* HFINTOSC at the reset OSCFRQ */

    hwaddr pie_addr;
    hwaddr pir_addr;
    hwaddr ipr_addr;
    hwaddr shad_addr;
    hwaddr osc_addr;
    hwaddr pmd_addr;
    hwaddr ivt_addr;
    hwaddr wwdt_addr;
    hwaddr tmr0_addr;
    hwaddr port_data_addr;
    hwaddr port_pad_addr;
    hwaddr pps_out_addr;
    hwaddr pps_in_addr;
    hwaddr spi1_addr;
    hwaddr spi2_addr;

    int irq_ioc;
    int irq_tmr0;
    int irq_spi1_rx;
    int irq_spi1_tx;
    int irq_spi1;
    int irq_spi2_rx;
    int irq_spi2_tx;
    int irq_spi2;
};

struct PIC18Q43SocState {
    SysBusDevice parent_obj;

    PIC18CPU cpu;
    Clock *fosc;
    int32_t osc_ppm;
    uint64_t base_fosc_hz;

    QEMUTimer *drift_timer;
    PIC18DriftProfile drift;

    MemoryRegion flash;
    MemoryRegion userid;
    MemoryRegion config;
    MemoryRegion eeprom;
    MemoryRegion gpr_lo;    /* 0x500..0x7FF, the rest of the first page */
    MemoryRegion gpr;       /* page-aligned from 0x800 */
    MemoryRegion intc;
    MemoryRegion ipr;
    MemoryRegion shad;
    MemoryRegion pps_out;
    MemoryRegion pps_in;
    MemoryRegion osc;
    MemoryRegion pmd;
    MemoryRegion ivt;

    PIC18PortState port;
    PIC18SpiState spi1;
    PIC18SpiState spi2;
    PIC18Tmr0State tmr0;
    PIC18WwdtState wwdt;

    uint8_t pir_latch[PIC18_NUM_PIR];
    uint8_t pir_level[PIC18_NUM_PIR];
    uint8_t pie[PIC18_NUM_PIR];
    uint8_t ipr_regs[PIC18_NUM_PIR];
    uint8_t pps_out_regs[PIC18_PPS_OUT_SIZE];
    uint8_t pps_in_regs[PIC18_PPS_IN_SIZE];
    uint8_t osc_regs[PIC18_OSC_NREGS];
    uint8_t pmd_regs[PIC18_PMD_NREGS];
    uint8_t ivt_regs[PIC18_IVT_NREGS];
    uint8_t shadcon;

    /* The level each SPI module drives on its SS(out). */
    bool spi_ss_level[2];

    qemu_irq cpu_irq_high;
    qemu_irq cpu_irq_low;
};

void pic18q43_soc_parse_drift(PIC18Q43SocState *s, const char *str);

#endif /* HW_PIC18_PIC18Q43_SOC_H */
