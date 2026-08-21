/*
 * PIC32 I/O ports
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32_GPIO_H
#define HW_PIC32_PIC32_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PIC32_GPIO "pic32-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(PIC32GpioState, PIC32_GPIO)

/* Ports A to G, sixteen pins each, whether or not the package bonds them out. */
#define PIC32_GPIO_PORTS 7
#define PIC32_GPIO_PINS 16
#define PIC32_GPIO_LINES (PIC32_GPIO_PORTS * PIC32_GPIO_PINS)

/* One line per pin, numbered port * 16 + pin, in both directions. */
#define PIC32_GPIO_OUT_GPIO "port-out"
#define PIC32_GPIO_IN_GPIO "port-in"

/* One change-notice interrupt line per port (sources 44..50 in the EVIC). */
#define PIC32_GPIO_CN_GPIO "cn"

/* 0x100 of register space per port. */
#define PIC32_GPIO_PORT_SIZE 0x100
#define PIC32_GPIO_SIZE (PIC32_GPIO_PORTS * PIC32_GPIO_PORT_SIZE)

struct PIC32GpioState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    uint32_t ansel[PIC32_GPIO_PORTS];
    uint32_t tris[PIC32_GPIO_PORTS];
    uint32_t lat[PIC32_GPIO_PORTS];
    uint32_t odc[PIC32_GPIO_PORTS];
    uint32_t cnpu[PIC32_GPIO_PORTS];
    uint32_t cnpd[PIC32_GPIO_PORTS];
    uint32_t cncon[PIC32_GPIO_PORTS];
    uint32_t cnen[PIC32_GPIO_PORTS];
    uint32_t cnne[PIC32_GPIO_PORTS];
    uint32_t cnstat[PIC32_GPIO_PORTS];
    uint32_t cnf[PIC32_GPIO_PORTS];

    /* Levels driven onto the pins from outside. */
    uint32_t input[PIC32_GPIO_PORTS];

    /* The pin levels the change notice last saw, for edge detection. */
    uint32_t cn_last[PIC32_GPIO_PORTS];

    qemu_irq out[PIC32_GPIO_LINES];
    qemu_irq cn[PIC32_GPIO_PORTS];
};

#endif /* HW_PIC32_PIC32_GPIO_H */
