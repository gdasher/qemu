/*
 * Microchip MCP23S08 8-bit SPI I/O expander
 *
 * A transaction is three bytes: an opcode carrying the device address and the
 * read/write bit, a register address, then the data. Chip select frames it, so
 * deasserting CS resets the sequence.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "hw/chips/mcp23s08.h"

enum {
    REG_IODIR,      /* 1 = input */
    REG_IPOL,       /* 1 = invert the sensed level */
    REG_GPINTEN,
    REG_DEFVAL,
    REG_INTCON,
    REG_IOCON,
    REG_GPPU,
    REG_INTF,       /* read-only */
    REG_INTCAP,     /* read-only, cleared by reading */
    REG_GPIO,
    REG_OLAT,
    MCP23S08_NREGS,
};

#define IOCON_INTPOL 0x02   /* 1 = INT is active high */

#define OPCODE_BASE 0x40
#define OPCODE_ADDR_MASK 0x06
#define OPCODE_ADDR_SHIFT 1
#define OPCODE_READ 0x01

static uint8_t mcp23s08_gpio(MCP23S08State *s)
{
    uint8_t in = s->input ^ s->regs[REG_IPOL];

    return (in & s->regs[REG_IODIR]) |
           (s->regs[REG_OLAT] & ~s->regs[REG_IODIR]);
}

static void mcp23s08_update_int(MCP23S08State *s)
{
    bool active = s->regs[REG_INTF] != 0;

    if (!(s->regs[REG_IOCON] & IOCON_INTPOL)) {
        active = !active;
    }
    qemu_set_irq(s->intr, active);
}

static void mcp23s08_set_pin(void *opaque, int line, int level)
{
    MCP23S08State *s = opaque;
    uint8_t mask = 1u << line;
    uint8_t before = mcp23s08_gpio(s);
    uint8_t after;

    if (level) {
        s->input |= mask;
    } else {
        s->input &= ~mask;
    }

    after = mcp23s08_gpio(s);
    if (((before ^ after) & mask & s->regs[REG_GPINTEN]) == 0) {
        return;
    }

    /*
     * Interrupt-on-change against the previous value. DEFVAL comparison
     * (INTCON) is not modelled; the reference board leaves INTCON clear.
     */
    s->regs[REG_INTF] |= mask;
    s->regs[REG_INTCAP] = after;
    mcp23s08_update_int(s);
}

static uint8_t mcp23s08_read_reg(MCP23S08State *s, uint8_t reg)
{
    switch (reg) {
    case REG_GPIO:
        /* Reading the port clears the interrupt it raised. */
        s->regs[REG_INTF] = 0;
        mcp23s08_update_int(s);
        return mcp23s08_gpio(s);
    case REG_INTCAP:
        s->regs[REG_INTF] = 0;
        mcp23s08_update_int(s);
        return s->regs[REG_INTCAP];
    default:
        return reg < MCP23S08_NREGS ? s->regs[reg] : 0;
    }
}

static void mcp23s08_write_reg(MCP23S08State *s, uint8_t reg, uint8_t value)
{
    switch (reg) {
    case REG_INTF:
    case REG_INTCAP:
        break;  /* read-only */
    case REG_GPIO:
    case REG_OLAT:
        s->regs[REG_OLAT] = value;
        break;
    default:
        if (reg < MCP23S08_NREGS) {
            s->regs[reg] = value;
        }
        break;
    }
    mcp23s08_update_int(s);
}

static uint32_t mcp23s08_transfer(SSIPeripheral *dev, uint32_t value)
{
    MCP23S08State *s = MCP23S08(dev);

    switch (s->phase) {
    case 0:
        if ((value & ~(OPCODE_READ | OPCODE_ADDR_MASK)) != OPCODE_BASE) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mcp23s08: unexpected opcode 0x%02x\n", value);
        }
        /*
         * The rest of the transaction belongs to whichever chip the opcode
         * addressed. One that was not addressed has to stay silent as well as
         * still, because the bus gives the controller every chip's output at
         * once and a second answer would corrupt the first.
         */
        s->addressed = ((value & OPCODE_ADDR_MASK) >> OPCODE_ADDR_SHIFT) ==
                       s->addr;
        s->reading = value & OPCODE_READ;
        s->phase = 1;
        return 0;
    case 1:
        s->reg = value;
        s->phase = 2;
        return 0;
    default:
        if (!s->addressed) {
            return 0;
        }
        if (s->reading) {
            return mcp23s08_read_reg(s, s->reg);
        }
        mcp23s08_write_reg(s, s->reg, value);
        /* Sequential mode would advance the address; the driver does not use it. */
        return 0;
    }
}

static int mcp23s08_set_cs(SSIPeripheral *dev, bool select)
{
    MCP23S08State *s = MCP23S08(dev);

    /* Every falling edge of CS starts a fresh transaction. */
    s->phase = 0;
    return 0;
}

static void mcp23s08_reset_hold(Object *obj, ResetType type)
{
    MCP23S08State *s = MCP23S08(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[REG_IODIR] = 0xFF;  /* all pins are inputs out of reset */
    s->phase = 0;
    s->reg = 0;
    s->reading = false;
    mcp23s08_update_int(s);
}

static void mcp23s08_realize(SSIPeripheral *dev, Error **errp)
{
    MCP23S08State *s = MCP23S08(dev);

    qdev_init_gpio_in_named(DEVICE(dev), mcp23s08_set_pin, MCP23S08_IN_GPIO,
                            MCP23S08_PINS);
    qdev_init_gpio_out_named(DEVICE(dev), &s->intr, MCP23S08_INT_GPIO, 1);
}

static const Property mcp23s08_properties[] = {
    DEFINE_PROP_UINT8("address", MCP23S08State, addr, 0),
};

static const VMStateDescription mcp23s08_vmstate = {
    .name = "mcp23s08",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, MCP23S08State),
        VMSTATE_UINT8_ARRAY(regs, MCP23S08State, MCP23S08_NREGS),
        VMSTATE_UINT8(input, MCP23S08State),
        VMSTATE_UINT8(reg, MCP23S08State),
        VMSTATE_UINT8(phase, MCP23S08State),
        VMSTATE_BOOL(addressed, MCP23S08State),
        VMSTATE_BOOL(reading, MCP23S08State),
        VMSTATE_END_OF_LIST()
    }
};

static void mcp23s08_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    k->realize = mcp23s08_realize;
    k->transfer = mcp23s08_transfer;
    k->set_cs = mcp23s08_set_cs;
    k->cs_polarity = SSI_CS_LOW;
    device_class_set_props(dc, mcp23s08_properties);
    dc->vmsd = &mcp23s08_vmstate;
    rc->phases.hold = mcp23s08_reset_hold;
}

static const TypeInfo mcp23s08_types[] = {
    {
        .name = TYPE_MCP23S08,
        .parent = TYPE_SSI_PERIPHERAL,
        .instance_size = sizeof(MCP23S08State),
        .class_init = mcp23s08_class_init,
    },
};

DEFINE_TYPES(mcp23s08_types)
