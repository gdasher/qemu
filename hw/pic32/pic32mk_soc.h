/*
 * PIC32MK SoC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32MK_SOC_H
#define HW_PIC32_PIC32MK_SOC_H

#include "hw/core/clock.h"
#include "hw/core/sysbus.h"
#include "hw/pic32/pic32_cru.h"
#include "hw/pic32/pic32_evic.h"
#include "hw/pic32/pic32_gpio.h"
#include "hw/pic32/pic32_pmp.h"
#include "hw/pic32/pic32_pps.h"
#include "hw/pic32/pic32_spi.h"
#include "hw/pic32/pic32_timer.h"
#include "hw/pic32/pic32_uart.h"
#include "hw/pic32/pic32_wdt.h"
#include "hw/pic32/pic32_dmac.h"
#include "qom/object.h"
#include "target/mips/cpu.h"

#define TYPE_PIC32MK_SOC "pic32mk-soc"
#define TYPE_PIC32MK1024GPK100_SOC "pic32mk1024gpk100-soc"
OBJECT_DECLARE_TYPE(PIC32MKSocState, PIC32MKSocClass, PIC32MK_SOC)

/*
 * Physical addresses. The core's fixed-mapping MMU turns KSEG0 and KSEG1 into
 * these by masking the top three bits, so the same memory answers at
 * 0x8xxxxxxx cached and 0xAxxxxxxx uncached with nothing here to arrange it.
 */
#define PIC32_RAM_BASE        0x00000000
#define PIC32_FLASH_BASE      0x1D000000
#define PIC32_SFR_BASE        0x1F800000
#define PIC32_SFR_SIZE        0x00100000
#define PIC32_BOOT_BASE       0x1FC00000

/* Peripherals, at the addresses the firmware's own symbol table gives. */
#define PIC32_CFG_BASE        0x1F800000
#define PIC32_WDT_BASE        0x1F800C00
#define PIC32_CRU_BASE        0x1F801200
#define PIC32_PPS_BASE        0x1F801400
#define PIC32_EVIC_BASE       0x1F810000
#define PIC32_TIMER1_BASE     0x1F820000
#define PIC32_SPI1_BASE       0x1F827000
#define PIC32_SPI3_BASE       0x1F847400
#define PIC32_SPI4_BASE       0x1F847600
#define PIC32_UART1_BASE      0x1F828000
#define PIC32_UART2_BASE      0x1F828200
#define PIC32_PMP_BASE        0x1F82E000
#define PIC32_GPIO_BASE       0x1F860000
#define PIC32_DMAC_BASE       0x1F811000

#define PIC32_NUM_UARTS 2
/*
 * SPI1 to SPI6 exist on the part; these are the three the board has anything
 * on -- the SD card, the port expanders, and SPI4, whose SDO can be routed to
 * the LED data pin -- and they are not adjacent, so they are placed one at a
 * time rather than as a run.
 */
#define PIC32_NUM_SPIS 3

/* Timers 1 to 3, 0x200 apart, starting at Timer1. */
#define PIC32_NUM_TIMERS 3
#define PIC32_TIMER_STRIDE 0x200

/* Which SPI controller the nth entry of the SoC's spi[] array is. */
unsigned pic32_spi_number(unsigned index);

/*
 * The modelled blocks a Peripheral Module Disable bit can stop, one flag
 * each, in the order the arrays above lay them out.
 */
enum {
    PIC32_PMD_GATE_TIMER1 = 0,                              /* + timer index */
    PIC32_PMD_GATE_UART1 = PIC32_NUM_TIMERS,                /* + uart index */
    PIC32_PMD_GATE_SPI1 = PIC32_PMD_GATE_UART1 + PIC32_NUM_UARTS, /* + spi */
    PIC32_PMD_GATE_PMP = PIC32_PMD_GATE_SPI1 + PIC32_NUM_SPIS,
    PIC32_PMD_GATE_DMAC,
    PIC32_PMD_GATE_COUNT
};

struct PIC32MKSocClass {
    SysBusDeviceClass parent_class;

    const char *cpu_type;
    uint64_t sysclk_hz;     /* what the configuration words' PLL settles at */
    unsigned pbclk_div;     /* SYSCLK to the peripheral bus the modules use */
    uint64_t ram_size;
    uint64_t flash_size;
    uint64_t boot_flash_size;
    uint32_t devid;
};

/* Whether the configuration words leave the watchdog running. */
void pic32mk_soc_set_watchdog(PIC32MKSocState *s, bool enabled);

struct PIC32MKSocState {
    SysBusDevice parent_obj;

    MIPSCPU *cpu;
    Clock *sysclk;
    Clock *pbclk;

    MemoryRegion ram;
    MemoryRegion flash;
    MemoryRegion boot_flash;

    PIC32CruState cru;
    PIC32PpsState pps;
    PIC32GpioState gpio;
    PIC32UartState uart[PIC32_NUM_UARTS];
    PIC32SpiState spi[PIC32_NUM_SPIS];
    PIC32TimerState timer[PIC32_NUM_TIMERS];
    PIC32EvicState evic;
    PIC32PmpState pmp;
    PIC32WdtState wdt;
    PIC32DmacState dmac;

    /* Watched by each block's register fabric; kept current from PMD1-7. */
    bool pmd_gate[PIC32_PMD_GATE_COUNT];
};

#endif /* HW_PIC32_PIC32MK_SOC_H */
