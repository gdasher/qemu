/*
 * PIC32 UART
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32_UART_H
#define HW_PIC32_PIC32_UART_H

#include "chardev/char-fe.h"
#include "hw/core/clock.h"
#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_PIC32_UART "pic32-uart"
OBJECT_DECLARE_SIMPLE_TYPE(PIC32UartState, PIC32_UART)

#define PIC32_UART_SIZE 0x50

/*
 * Each UART owns three interrupt sources, in the order the EVIC numbers them:
 * a fault, then receive, then transmit.
 */
#define PIC32_UART_IRQ_GPIO "irq"
enum {
    PIC32_UART_IRQ_FAULT,
    PIC32_UART_IRQ_RX,
    PIC32_UART_IRQ_TX,
    PIC32_UART_IRQS,
};

/* The receive FIFO is eight deep. */
#define PIC32_UART_FIFO 8

struct PIC32UartState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    CharFrontend chr;
    Clock *pbclk;

    uint32_t mode;
    uint32_t sta;
    uint32_t brg;

    uint8_t rx[PIC32_UART_FIFO];
    uint32_t rx_count;

    qemu_irq irq[PIC32_UART_IRQS];
};

#endif /* HW_PIC32_PIC32_UART_H */
