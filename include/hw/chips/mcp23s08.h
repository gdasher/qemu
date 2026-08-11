/*
 * Microchip MCP23S08 8-bit SPI I/O expander
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHIPS_MCP23S08_H
#define HW_CHIPS_MCP23S08_H

#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_MCP23S08 "mcp23s08"
OBJECT_DECLARE_SIMPLE_TYPE(MCP23S08State, MCP23S08)

#define MCP23S08_PINS 8
#define MCP23S08_IN_GPIO "gp"
#define MCP23S08_OUT_GPIO "out"
#define MCP23S08_INT_GPIO "int"

struct MCP23S08State {
    SSIPeripheral parent_obj;

    /*
     * The two address pins. Up to four of these can share one chip select,
     * each answering only to opcodes carrying its own address, which is how
     * a board fits more than eight lines on one select.
     */
    uint8_t addr;

    uint8_t regs[11];
    uint8_t input;      /* levels driven onto the pins from outside */

    /* Transaction state: opcode, register, then data. */
    uint8_t reg;
    uint8_t phase;
    bool reading;
    bool addressed;     /* the opcode named this chip */

    qemu_irq intr;

    /*
     * The pins, as outputs. A pin the direction register calls an input is
     * not driven by the part at all; there is no level to hand on for one, so
     * these carry what the latch puts on the pins that are outputs and read
     * low for the rest.
     */
    qemu_irq out[MCP23S08_PINS];
    uint8_t driven;
};

#endif /* HW_CHIPS_MCP23S08_H */
