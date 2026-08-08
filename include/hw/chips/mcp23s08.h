/*
 * Microchip MCP23S08 8-bit SPI I/O expander
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_MCP23S08_H
#define HW_PIC16_MCP23S08_H

#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_MCP23S08 "mcp23s08"
OBJECT_DECLARE_SIMPLE_TYPE(MCP23S08State, MCP23S08)

#define MCP23S08_PINS 8
#define MCP23S08_IN_GPIO "gp"
#define MCP23S08_INT_GPIO "int"

struct MCP23S08State {
    SSIPeripheral parent_obj;

    uint8_t regs[11];
    uint8_t input;      /* levels driven onto the pins from outside */

    /* Transaction state: opcode, register, then data. */
    uint8_t reg;
    uint8_t phase;
    bool reading;

    qemu_irq intr;
};

#endif /* HW_PIC16_MCP23S08_H */
