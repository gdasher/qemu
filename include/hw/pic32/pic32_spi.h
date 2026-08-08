/*
 * PIC32 SPI controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32_SPI_H
#define HW_PIC32_PIC32_SPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_PIC32_SPI "pic32-spi"
OBJECT_DECLARE_SIMPLE_TYPE(PIC32SpiState, PIC32_SPI)

#define PIC32_SPI_SIZE 0x50

/*
 * Three interrupt sources per controller, in the order the EVIC numbers them:
 * a fault, then receive, then transmit.
 */
#define PIC32_SPI_IRQ_GPIO "irq"
enum {
    PIC32_SPI_IRQ_FAULT,
    PIC32_SPI_IRQ_RX,
    PIC32_SPI_IRQ_TX,
    PIC32_SPI_IRQS,
};

/* The enhanced buffer holds 128 bits: sixteen bytes, or four words. */
#define PIC32_SPI_FIFO 16

struct PIC32SpiState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    SSIBus *ssi;

    uint32_t con;
    uint32_t con2;
    uint32_t stat;
    uint32_t brg;

    uint32_t rx[PIC32_SPI_FIFO];
    uint32_t rx_count;

    qemu_irq irq[PIC32_SPI_IRQS];
};

#endif /* HW_PIC32_PIC32_SPI_H */
