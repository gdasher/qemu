/*
 * PIC18F-Q43 I/O ports
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC18_PORT_H
#define HW_PIC18_PORT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PIC18_PORT "pic18-port"
OBJECT_DECLARE_SIMPLE_TYPE(PIC18PortState, PIC18_PORT)

/* Ports A, B, C and E (RE3 only, the MCLR/VPP input). */
#define PIC18_PORTS 4
#define PIC18_PORT_E 3
#define PIC18_PORT_PINS 8
#define PIC18_PORT_LINES (PIC18_PORTS * PIC18_PORT_PINS)

/* Named GPIO array for driving a pin from outside. */
#define PIC18_PORT_IN_GPIO "pin"

struct PIC18PortState {
    SysBusDevice parent_obj;

    MemoryRegion iomem_data;    /* LATx, TRISx, PORTx block at 0x4BE */
    MemoryRegion iomem_pad;     /* ANSELx .. IOCxF block at 0x400 */

    uint8_t pins[PIC18_PORTS];  /* implemented pins per port */

    uint8_t lat[PIC18_PORTS];
    uint8_t tris[PIC18_PORTS];
    uint8_t ansel[PIC18_PORTS];
    uint8_t wpu[PIC18_PORTS];
    uint8_t odcon[PIC18_PORTS];
    uint8_t slrcon[PIC18_PORTS];
    uint8_t inlvl[PIC18_PORTS];
    uint8_t iocp[PIC18_PORTS];
    uint8_t iocn[PIC18_PORTS];
    uint8_t iocf[PIC18_PORTS];

    /* Levels driven onto the pins from outside. */
    uint8_t input[PIC18_PORTS];

    /*
     * Peripheral Pin Select: a pin whose RxyPPS names a peripheral output
     * drives that signal instead of its latch bit. The SoC owns the PPS
     * registers and tells the port which pins are routed and to what level.
     */
    uint8_t pps_mask[PIC18_PORTS];
    uint8_t pps_level[PIC18_PORTS];

    qemu_irq out[PIC18_PORT_LINES];
    qemu_irq ioc_irq;
};

/**
 * pic18_port_set_peripheral_output:
 * @line: port * 8 + pin
 * @routed: the pin follows a peripheral signal rather than LATx
 * @level: the peripheral signal's current level
 *
 * Re-evaluates the pin at once, so a PPS change or a chip-select edge from a
 * peripheral reaches whatever is wired to the pin without waiting for a LAT
 * write.
 */
void pic18_port_set_peripheral_output(PIC18PortState *s, unsigned line,
                                      bool routed, bool level);

#endif /* HW_PIC18_PORT_H */
