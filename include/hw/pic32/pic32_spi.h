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
#include "qemu/timer.h"
#include "hw/core/clock.h"

#define TYPE_PIC32_SPI "pic32-spi"
OBJECT_DECLARE_SIMPLE_TYPE(PIC32SpiState, PIC32_SPI)

#define PIC32_SPI_SIZE 0x50

/*
 * Three interrupt sources per controller, in the order the EVIC numbers them:
 * a fault, then receive, then transmit.
 */
#define PIC32_SPI_IRQ_GPIO "irq"

/*
 * The serial output as a pin rather than as a bus. A controller whose SDO is
 * routed to a port pin drives this line one bit at a time at the baud rate,
 * which is the only way a device that decodes edge timing -- an addressable
 * LED strip on the other end of the pin -- can be driven from a transfer.
 */
#define PIC32_SPI_SDO_GPIO "sdo"
enum {
    PIC32_SPI_IRQ_FAULT,
    PIC32_SPI_IRQ_RX,
    PIC32_SPI_IRQ_TX,
    PIC32_SPI_IRQS,
};

/*
 * The enhanced buffer holds 128 bits -- sixteen bytes, eight half-words or
 * four words -- so this is the backing store's size, not the depth the guest
 * sees; that depends on the word width the controller is set for.
 */
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
    uint32_t tx[PIC32_SPI_FIFO];
    uint32_t tx_count;

    /* The bit being shifted out, when the output is a pin. */
    bool serial_out;
    Clock *pbclk;
    QEMUTimer *shift;
    uint32_t shift_reg;
    uint32_t shift_bits;
    bool sdo_level;
    qemu_irq sdo;
    uint32_t rx_count;

    /* One-shot diagnostics; not guest-visible state, so not migrated. */
    bool srxisel_logged;
    bool stxisel_logged;

    qemu_irq irq[PIC32_SPI_IRQS];
};

#endif /* HW_PIC32_PIC32_SPI_H */
