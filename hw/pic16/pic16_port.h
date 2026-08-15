/*
 * PIC16 I/O ports
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_PORT_H
#define HW_PIC16_PORT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PIC16_PORT "pic16-port"
OBJECT_DECLARE_SIMPLE_TYPE(PIC16PortState, PIC16_PORT)

#define PIC16_PORTS 3
#define PIC16_PORT_PINS 8
#define PIC16_PORT_LINES (PIC16_PORTS * PIC16_PORT_PINS)

/* Named GPIO array for driving a pin from outside. */
#define PIC16_PORT_IN_GPIO "pin"

struct PIC16PortState {
    SysBusDevice parent_obj;

    MemoryRegion iomem_data;    /* bank 0: PORTx, TRISx, LATx */
    MemoryRegion iomem_pad;     /* bank 61/62: ANSELx .. IOCxF */

    uint8_t layout;             /* 0: PIC16F175xx, 1: PIC16F153xx */

    uint8_t lat[PIC16_PORTS];
    uint8_t tris[PIC16_PORTS];
    uint8_t ansel[PIC16_PORTS];
    uint8_t wpu[PIC16_PORTS];
    uint8_t odcon[PIC16_PORTS];
    uint8_t slrcon[PIC16_PORTS];
    uint8_t inlvl[PIC16_PORTS];
    uint8_t iocp[PIC16_PORTS];
    uint8_t iocn[PIC16_PORTS];
    uint8_t iocf[PIC16_PORTS];

    /* Levels driven onto the pins from outside. */
    uint8_t input[PIC16_PORTS];

    qemu_irq out[PIC16_PORT_LINES];
    qemu_irq ioc_irq;
};

#endif /* HW_PIC16_PORT_H */
