/*
 * PIC16 I/O ports, with interrupt-on-change
 *
 * The registers for a port are not contiguous: PORTx, TRISx and LATx sit in
 * three separate runs of bank 0, while the pad control and interrupt-on-change
 * registers are grouped per port in bank 61. One device covers all the ports
 * with a region for each of those two areas.
 *
 * Port E, where the layout has one, is only RE3: the MCLR/VPP pin read back as
 * an input. It has weak pull-up, input level and interrupt-on-change controls
 * but no output driver, so TRISE reads as 1 and LATE is unimplemented.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "pic16_port.h"

/* Offsets within the bank 0 region, which starts at PORTA. */
#define PORT_REG_PORT 0
#define PORT_REG_TRIS 6
#define PORT_REG_LAT  12
#define PORT_REGS_ABC (PORT_REG_LAT + 3)

/* PORTE, TRISE and LATE on the PIC16F153xx layout, which has a port E. */
#define PORT_REG_PORTE 0x04
#define PORT_REG_TRISE 0x0A
#define PORT_REG_LATE  0x10
#define PORT_REGS_ABCE (PORT_REG_LATE + 1)

/* Which pins each layout implements. */
static const uint8_t pic16_port_layout_pins[][PIC16_PORTS] = {
    [0] = { 0xFF, 0xFF, 0xFF, 0x00 },   /* PIC16F175xx: no port E */
    [1] = { 0xFF, 0xFF, 0xFF, 0x08 },   /* PIC16F153xx: RE3 only */
};

/* Registers per port within the bank 61 region, which starts at ANSELA. */
#define PAD_REGS_PER_PORT 10
enum {
    PAD_REG_ANSEL,
    PAD_REG_WPU,
    PAD_REG_ODCON,
    PAD_REG_SLRCON,
    PAD_REG_INLVL,
    PAD_REG_IOCP,
    PAD_REG_IOCN,
    PAD_REG_IOCF,
};

/* Reading a port returns the pin levels: driven where an output, sensed where not. */
static uint8_t pic16_port_pins(PIC16PortState *s, unsigned p)
{
    return ((s->lat[p] & ~s->tris[p]) | (s->input[p] & s->tris[p])) &
           s->pins[p];
}

static void pic16_port_update_outputs(PIC16PortState *s, unsigned p)
{
    unsigned pin;

    for (pin = 0; pin < PIC16_PORT_PINS; pin++) {
        if (!(s->tris[p] & (1u << pin)) && (s->pins[p] & (1u << pin))) {
            qemu_set_irq(s->out[p * PIC16_PORT_PINS + pin],
                         (s->lat[p] >> pin) & 1);
        }
    }
}

/*
 * IOCxF is a latch, but the interrupt flag it feeds is not: PIR0's IOCIF
 * simply reflects whether any IOCxF bit is set, which is why clearing IOCAF in
 * the handler is enough to dismiss the interrupt.
 */
static void pic16_port_update_ioc(PIC16PortState *s)
{
    unsigned p;

    for (p = 0; p < PIC16_PORTS; p++) {
        if (s->iocf[p]) {
            qemu_set_irq(s->ioc_irq, 1);
            return;
        }
    }
    qemu_set_irq(s->ioc_irq, 0);
}

static void pic16_port_set_pin(void *opaque, int line, int level)
{
    PIC16PortState *s = opaque;
    unsigned p = line / PIC16_PORT_PINS;
    unsigned pin = line % PIC16_PORT_PINS;
    uint8_t mask = 1u << pin;
    bool was = s->input[p] & mask;

    if (level) {
        s->input[p] |= mask;
    } else {
        s->input[p] &= ~mask;
    }

    /* Edges are only detected on pins configured as inputs. */
    if (!(s->tris[p] & mask) || (bool)level == was) {
        return;
    }
    if (level ? (s->iocp[p] & mask) : (s->iocn[p] & mask)) {
        s->iocf[p] |= mask;
        pic16_port_update_ioc(s);
    }
}

/*
 * "input-a" and friends expose the levels driven onto each port from outside,
 * so a harness can poke a pin over QMP without a device in between:
 *
 *   qom-set /machine/soc/port input-b 0x20
 *
 * Writes go through the same path as a device driving the line, so edges are
 * detected and interrupt-on-change behaves identically.
 */
static void pic16_port_get_input(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    PIC16PortState *s = PIC16_PORT(obj);
    uint8_t value = s->input[(uintptr_t)opaque];

    visit_type_uint8(v, name, &value, errp);
}

static void pic16_port_set_input(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    PIC16PortState *s = PIC16_PORT(obj);
    unsigned p = (uintptr_t)opaque;
    uint8_t value;
    unsigned pin;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    for (pin = 0; pin < PIC16_PORT_PINS; pin++) {
        pic16_port_set_pin(s, p * PIC16_PORT_PINS + pin,
                           (value >> pin) & 1);
    }
}

static bool pic16_port_has_e(PIC16PortState *s)
{
    return s->pins[PIC16_PORT_E] != 0;
}

static uint64_t pic16_port_data_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16PortState *s = opaque;

    if (addr < PORT_REG_PORT + 3) {
        return pic16_port_pins(s, addr - PORT_REG_PORT);
    }
    if (addr >= PORT_REG_TRIS && addr < PORT_REG_TRIS + 3) {
        return s->tris[addr - PORT_REG_TRIS];
    }
    if (addr >= PORT_REG_LAT && addr < PORT_REG_LAT + 3) {
        return s->lat[addr - PORT_REG_LAT];
    }
    if (pic16_port_has_e(s)) {
        switch (addr) {
        case PORT_REG_PORTE:
            return pic16_port_pins(s, PIC16_PORT_E);
        case PORT_REG_TRISE:
            /* Input only: the implemented bit reads as 1. */
            return s->pins[PIC16_PORT_E];
        case PORT_REG_LATE:
        default:
            return 0;
        }
    }
    return 0;
}

static void pic16_port_data_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    PIC16PortState *s = opaque;
    unsigned p;

    if (addr < PORT_REG_PORT + 3) {
        /* Writing a port writes its latch. */
        p = addr - PORT_REG_PORT;
        s->lat[p] = value;
    } else if (addr >= PORT_REG_TRIS && addr < PORT_REG_TRIS + 3) {
        p = addr - PORT_REG_TRIS;
        s->tris[p] = value;
    } else if (addr >= PORT_REG_LAT && addr < PORT_REG_LAT + 3) {
        p = addr - PORT_REG_LAT;
        s->lat[p] = value;
    } else {
        /* PORTE, TRISE and LATE have nothing writable. */
        return;
    }
    pic16_port_update_outputs(s, p);
}

static const MemoryRegionOps pic16_port_data_ops = {
    .read = pic16_port_data_read,
    .write = pic16_port_data_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static bool pic16_port_decode_pad_addr(PIC16PortState *s, hwaddr addr,
                                       unsigned *port_out, unsigned *reg_out)
{
    if (s->layout == 1) { /* PIC16F153xx layout */
        if (addr <= 0x07) {
            *port_out = 0; /* Port A */
            *reg_out = addr;
            return true;
        } else if (addr >= 0x0B && addr <= 0x12) {
            *port_out = 1; /* Port B */
            *reg_out = addr - 0x0B;
            return true;
        } else if (addr >= 0x16 && addr <= 0x1D) {
            *port_out = 2; /* Port C */
            *reg_out = addr - 0x16;
            return true;
        } else if (addr == 0x2D) {
            *port_out = PIC16_PORT_E;
            *reg_out = PAD_REG_WPU;
            return true;
        } else if (addr == 0x30) {
            *port_out = PIC16_PORT_E;
            *reg_out = PAD_REG_INLVL;
            return true;
        } else if (addr >= 0x31 && addr <= 0x33) {
            *port_out = PIC16_PORT_E;
            *reg_out = PAD_REG_IOCP + (addr - 0x31);
            return true;
        }
        return false;
    } else { /* PIC16F175xx layout */
        unsigned p = addr / PAD_REGS_PER_PORT;
        unsigned reg = addr % PAD_REGS_PER_PORT;
        if (p < PIC16_PORTS && reg < 8 && s->pins[p]) {
            *port_out = p;
            *reg_out = reg;
            return true;
        }
        return false;
    }
}

static uint64_t pic16_port_pad_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16PortState *s = opaque;
    unsigned p, reg;

    if (!pic16_port_decode_pad_addr(s, addr, &p, &reg)) {
        return 0;
    }
    switch (reg) {
    case PAD_REG_ANSEL:
        return s->ansel[p];
    case PAD_REG_WPU:
        return s->wpu[p];
    case PAD_REG_ODCON:
        return s->odcon[p];
    case PAD_REG_SLRCON:
        return s->slrcon[p];
    case PAD_REG_INLVL:
        return s->inlvl[p];
    case PAD_REG_IOCP:
        return s->iocp[p];
    case PAD_REG_IOCN:
        return s->iocn[p];
    case PAD_REG_IOCF:
        return s->iocf[p];
    default:
        return 0;
    }
}

static void pic16_port_pad_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    PIC16PortState *s = opaque;
    unsigned p, reg;

    if (!pic16_port_decode_pad_addr(s, addr, &p, &reg)) {
        return;
    }
    /* Unimplemented bits read as 0. */
    value &= s->pins[p];
    switch (reg) {
    case PAD_REG_ANSEL:
        s->ansel[p] = value;
        break;
    case PAD_REG_WPU:
        s->wpu[p] = value;
        break;
    case PAD_REG_ODCON:
        s->odcon[p] = value;
        break;
    case PAD_REG_SLRCON:
        s->slrcon[p] = value;
        break;
    case PAD_REG_INLVL:
        s->inlvl[p] = value;
        break;
    case PAD_REG_IOCP:
        s->iocp[p] = value;
        break;
    case PAD_REG_IOCN:
        s->iocn[p] = value;
        break;
    case PAD_REG_IOCF:
        /* Software clears the flags it has handled; it cannot set them. */
        s->iocf[p] &= value;
        pic16_port_update_ioc(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pic16_port_pad_ops = {
    .read = pic16_port_pad_read,
    .write = pic16_port_pad_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic16_port_reset_hold(Object *obj, ResetType type)
{
    PIC16PortState *s = PIC16_PORT(obj);

    memset(s->lat, 0, sizeof(s->lat));
    memset(s->ansel, 0xFF, sizeof(s->ansel));  /* pins start analog */
    memset(s->tris, 0xFF, sizeof(s->tris));    /* and as inputs */
    memset(s->wpu, 0, sizeof(s->wpu));
    memset(s->odcon, 0, sizeof(s->odcon));
    memset(s->slrcon, 0xFF, sizeof(s->slrcon));
    memset(s->inlvl, 0xFF, sizeof(s->inlvl));
    memset(s->iocp, 0, sizeof(s->iocp));
    memset(s->iocn, 0, sizeof(s->iocn));
    memset(s->iocf, 0, sizeof(s->iocf));
    for (unsigned p = 0; p < PIC16_PORTS; p++) {
        s->ansel[p] &= s->pins[p];
        s->slrcon[p] &= s->pins[p];
        s->inlvl[p] &= s->pins[p];
    }
    qemu_set_irq(s->ioc_irq, 0);
}

static void pic16_port_realize(DeviceState *dev, Error **errp)
{
    PIC16PortState *s = PIC16_PORT(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (s->layout >= ARRAY_SIZE(pic16_port_layout_pins)) {
        error_setg(errp, "pic16-port: unknown layout %u", s->layout);
        return;
    }
    memcpy(s->pins, pic16_port_layout_pins[s->layout], sizeof(s->pins));

    memory_region_init_io(&s->iomem_data, OBJECT(dev), &pic16_port_data_ops, s,
                          "pic16.port.data",
                          pic16_port_has_e(s) ? PORT_REGS_ABCE : PORT_REGS_ABC);
    sysbus_init_mmio(sbd, &s->iomem_data);

    memory_region_init_io(&s->iomem_pad, OBJECT(dev), &pic16_port_pad_ops, s,
                          "pic16.port.pad", 0x40);
    sysbus_init_mmio(sbd, &s->iomem_pad);

    for (unsigned p = 0; p < PIC16_PORTS; p++) {
        g_autofree char *name =
            g_strdup_printf("input-%c", p == PIC16_PORT_E ? 'e' : 'a' + p);

        object_property_add(OBJECT(dev), name, "uint8",
                            pic16_port_get_input, pic16_port_set_input,
                            NULL, (void *)(uintptr_t)p);
    }

    qdev_init_gpio_out(dev, s->out, PIC16_PORT_LINES);
    qdev_init_gpio_in_named(dev, pic16_port_set_pin, PIC16_PORT_IN_GPIO,
                            PIC16_PORT_LINES);
    sysbus_init_irq(sbd, &s->ioc_irq);
}

static const Property pic16_port_properties[] = {
    DEFINE_PROP_UINT8("layout", PIC16PortState, layout, 0),
};

static const VMStateDescription pic16_port_vmstate = {
    .name = "pic16-port",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(layout, PIC16PortState),
        VMSTATE_UINT8_ARRAY(lat, PIC16PortState, PIC16_PORTS),
        VMSTATE_UINT8_ARRAY(tris, PIC16PortState, PIC16_PORTS),
        VMSTATE_UINT8_ARRAY(ansel, PIC16PortState, PIC16_PORTS),
        VMSTATE_UINT8_ARRAY(wpu, PIC16PortState, PIC16_PORTS),
        VMSTATE_UINT8_ARRAY(odcon, PIC16PortState, PIC16_PORTS),
        VMSTATE_UINT8_ARRAY(slrcon, PIC16PortState, PIC16_PORTS),
        VMSTATE_UINT8_ARRAY(inlvl, PIC16PortState, PIC16_PORTS),
        VMSTATE_UINT8_ARRAY(iocp, PIC16PortState, PIC16_PORTS),
        VMSTATE_UINT8_ARRAY(iocn, PIC16PortState, PIC16_PORTS),
        VMSTATE_UINT8_ARRAY(iocf, PIC16PortState, PIC16_PORTS),
        VMSTATE_UINT8_ARRAY(input, PIC16PortState, PIC16_PORTS),
        VMSTATE_END_OF_LIST()
    }
};

static void pic16_port_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic16_port_realize;
    dc->vmsd = &pic16_port_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, pic16_port_properties);
    rc->phases.hold = pic16_port_reset_hold;
}

static const TypeInfo pic16_port_types[] = {
    {
        .name = TYPE_PIC16_PORT,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC16PortState),
        .class_init = pic16_port_class_init,
    },
};

DEFINE_TYPES(pic16_port_types)
