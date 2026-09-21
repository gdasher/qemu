/*
 * PIC18F-Q43 I/O ports, with interrupt-on-change and PPS-routed outputs
 *
 * On the Q43 the data registers sit in one run: LATA/B/C(/E) at 0x4BE,
 * TRISA/B/C/E at 0x4C6, PORTA/B/C/E at 0x4CE (each group leaves a slot for
 * the port D the 28-pin part does not have). The pad-control registers are
 * grouped eight per port from 0x400: ANSEL, WPU, ODCON, SLRCON, INLVL, IOCxP,
 * IOCxN, IOCxF, with port E at 0x420.
 *
 * Port E is only RE3, the MCLR/VPP pin read back as an input: weak pull-up,
 * input level and interrupt-on-change controls, no output driver, so TRISE
 * reads as 1 and LATE is unimplemented.
 *
 * A pin's output is its LATx bit unless the SoC's PPS routing has handed the
 * pin to a peripheral (see pic18_port_set_peripheral_output()); the firmware
 * this was written for switches RA3 between SPI2's SS(out) and its latch at
 * run time, so the routing is live rather than a reset-time table.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "pic18_port.h"

/* Offsets within the data region, which starts at LATA (0x4BE). */
#define PORT_REG_LAT   0x00     /* LATA, LATB, LATC; LATE would be +4 */
#define PORT_REG_TRIS  0x08     /* TRISA..TRISC, TRISE at +0x0C */
#define PORT_REG_TRISE 0x0C
#define PORT_REG_PORT  0x10     /* PORTA..PORTC, PORTE at +0x14 */
#define PORT_REG_PORTE 0x14
#define PORT_DATA_SIZE 0x15

/* The pad region: eight registers per port, port E at slot 4 (0x420). */
#define PAD_REGS_PER_PORT 8
#define PAD_SLOT_E 4
#define PORT_PAD_SIZE 0x28
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

static const uint8_t pic18_port_pins_28pin[PIC18_PORTS] = {
    0xFF, 0xFF, 0xFF, 0x08,
};

/* What the output driver would put on each pin: the latch, or the routed
 * peripheral signal. */
static uint8_t pic18_port_drive(PIC18PortState *s, unsigned p)
{
    return (s->lat[p] & ~s->pps_mask[p]) | (s->pps_level[p] & s->pps_mask[p]);
}

/* Reading a port returns the pin levels: driven where an output, sensed
 * where not. */
static uint8_t pic18_port_read_pins(PIC18PortState *s, unsigned p)
{
    return ((pic18_port_drive(s, p) & ~s->tris[p]) |
            (s->input[p] & s->tris[p])) & s->pins[p];
}

static void pic18_port_update_outputs(PIC18PortState *s, unsigned p)
{
    uint8_t drive = pic18_port_drive(s, p);
    unsigned pin;

    for (pin = 0; pin < PIC18_PORT_PINS; pin++) {
        if (!(s->tris[p] & (1u << pin)) && (s->pins[p] & (1u << pin))) {
            qemu_set_irq(s->out[p * PIC18_PORT_PINS + pin],
                         (drive >> pin) & 1);
        }
    }
}

void pic18_port_set_peripheral_output(PIC18PortState *s, unsigned line,
                                      bool routed, bool level)
{
    unsigned p = line / PIC18_PORT_PINS;
    uint8_t mask = 1u << (line % PIC18_PORT_PINS);

    if (p >= PIC18_PORTS) {
        return;
    }
    if (routed) {
        s->pps_mask[p] |= mask;
    } else {
        s->pps_mask[p] &= ~mask;
    }
    if (level) {
        s->pps_level[p] |= mask;
    } else {
        s->pps_level[p] &= ~mask;
    }
    if (!(s->tris[p] & mask) && (s->pins[p] & mask)) {
        qemu_set_irq(s->out[line], (pic18_port_drive(s, p) & mask) != 0);
    }
}

/*
 * IOCxF is a latch, but the interrupt flag it feeds is not: PIR0's IOCIF
 * simply reflects whether any IOCxF bit is set, which is why clearing IOCCF
 * in the handler is enough to dismiss the interrupt.
 */
static void pic18_port_update_ioc(PIC18PortState *s)
{
    unsigned p;

    for (p = 0; p < PIC18_PORTS; p++) {
        if (s->iocf[p]) {
            qemu_set_irq(s->ioc_irq, 1);
            return;
        }
    }
    qemu_set_irq(s->ioc_irq, 0);
}

static void pic18_port_set_pin(void *opaque, int line, int level)
{
    PIC18PortState *s = opaque;
    unsigned p = line / PIC18_PORT_PINS;
    unsigned pin = line % PIC18_PORT_PINS;
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
        pic18_port_update_ioc(s);
    }
}

/*
 * "input-a" and friends expose the levels driven onto each port from outside,
 * so a harness can poke a pin over QMP without a device in between:
 *
 *   qom-set /machine/soc-core/port input-c 0x40
 *
 * Writes go through the same path as a device driving the line, so edges are
 * detected and interrupt-on-change behaves identically.
 */
static void pic18_port_get_input(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    PIC18PortState *s = PIC18_PORT(obj);
    uint8_t value = s->input[(uintptr_t)opaque];

    visit_type_uint8(v, name, &value, errp);
}

static void pic18_port_set_input(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    PIC18PortState *s = PIC18_PORT(obj);
    unsigned p = (uintptr_t)opaque;
    uint8_t value;
    unsigned pin;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    for (pin = 0; pin < PIC18_PORT_PINS; pin++) {
        pic18_port_set_pin(s, p * PIC18_PORT_PINS + pin, (value >> pin) & 1);
    }
}

static uint64_t pic18_port_data_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC18PortState *s = opaque;

    if (addr < PORT_REG_LAT + 3) {
        return s->lat[addr - PORT_REG_LAT];
    }
    if (addr >= PORT_REG_TRIS && addr < PORT_REG_TRIS + 3) {
        return s->tris[addr - PORT_REG_TRIS];
    }
    if (addr >= PORT_REG_PORT && addr < PORT_REG_PORT + 3) {
        return pic18_port_read_pins(s, addr - PORT_REG_PORT);
    }
    switch (addr) {
    case PORT_REG_TRISE:
        /* Input only: the implemented bit reads as 1. */
        return s->pins[PIC18_PORT_E];
    case PORT_REG_PORTE:
        return pic18_port_read_pins(s, PIC18_PORT_E);
    default:
        return 0;
    }
}

static void pic18_port_data_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    PIC18PortState *s = opaque;
    unsigned p;

    if (addr < PORT_REG_LAT + 3) {
        p = addr - PORT_REG_LAT;
        s->lat[p] = value;
    } else if (addr >= PORT_REG_TRIS && addr < PORT_REG_TRIS + 3) {
        p = addr - PORT_REG_TRIS;
        s->tris[p] = value;
    } else if (addr >= PORT_REG_PORT && addr < PORT_REG_PORT + 3) {
        /* Writing a port writes its latch. */
        p = addr - PORT_REG_PORT;
        s->lat[p] = value;
    } else {
        /* LATE, TRISE and PORTE have nothing writable. */
        return;
    }
    pic18_port_update_outputs(s, p);
}

static const MemoryRegionOps pic18_port_data_ops = {
    .read = pic18_port_data_read,
    .write = pic18_port_data_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static bool pic18_port_decode_pad_addr(PIC18PortState *s, hwaddr addr,
                                       unsigned *port_out, unsigned *reg_out)
{
    unsigned slot = addr / PAD_REGS_PER_PORT;
    unsigned reg = addr % PAD_REGS_PER_PORT;

    if (slot < 3) {
        *port_out = slot;
    } else if (slot == PAD_SLOT_E) {
        *port_out = PIC18_PORT_E;
    } else {
        return false;
    }
    *reg_out = reg;
    return s->pins[*port_out] != 0;
}

static uint64_t pic18_port_pad_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC18PortState *s = opaque;
    unsigned p, reg;

    if (!pic18_port_decode_pad_addr(s, addr, &p, &reg)) {
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

static void pic18_port_pad_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    PIC18PortState *s = opaque;
    unsigned p, reg;

    if (!pic18_port_decode_pad_addr(s, addr, &p, &reg)) {
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
        pic18_port_update_ioc(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pic18_port_pad_ops = {
    .read = pic18_port_pad_read,
    .write = pic18_port_pad_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic18_port_reset_hold(Object *obj, ResetType type)
{
    PIC18PortState *s = PIC18_PORT(obj);

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
    /* PPS routing resets with the SoC's RxyPPS registers, which re-route. */
    memset(s->pps_mask, 0, sizeof(s->pps_mask));
    memset(s->pps_level, 0, sizeof(s->pps_level));
    for (unsigned p = 0; p < PIC18_PORTS; p++) {
        s->ansel[p] &= s->pins[p];
        s->slrcon[p] &= s->pins[p];
        s->inlvl[p] &= s->pins[p];
    }
    qemu_set_irq(s->ioc_irq, 0);
}

static void pic18_port_realize(DeviceState *dev, Error **errp)
{
    PIC18PortState *s = PIC18_PORT(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memcpy(s->pins, pic18_port_pins_28pin, sizeof(s->pins));

    memory_region_init_io(&s->iomem_data, OBJECT(dev), &pic18_port_data_ops, s,
                          "pic18.port.data", PORT_DATA_SIZE);
    sysbus_init_mmio(sbd, &s->iomem_data);

    memory_region_init_io(&s->iomem_pad, OBJECT(dev), &pic18_port_pad_ops, s,
                          "pic18.port.pad", PORT_PAD_SIZE);
    sysbus_init_mmio(sbd, &s->iomem_pad);

    for (unsigned p = 0; p < PIC18_PORTS; p++) {
        g_autofree char *name =
            g_strdup_printf("input-%c", p == PIC18_PORT_E ? 'e' : 'a' + p);

        object_property_add(OBJECT(dev), name, "uint8",
                            pic18_port_get_input, pic18_port_set_input,
                            NULL, (void *)(uintptr_t)p);
    }

    qdev_init_gpio_out(dev, s->out, PIC18_PORT_LINES);
    qdev_init_gpio_in_named(dev, pic18_port_set_pin, PIC18_PORT_IN_GPIO,
                            PIC18_PORT_LINES);
    sysbus_init_irq(sbd, &s->ioc_irq);
}

static const VMStateDescription pic18_port_vmstate = {
    .name = "pic18-port",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(lat, PIC18PortState, PIC18_PORTS),
        VMSTATE_UINT8_ARRAY(tris, PIC18PortState, PIC18_PORTS),
        VMSTATE_UINT8_ARRAY(ansel, PIC18PortState, PIC18_PORTS),
        VMSTATE_UINT8_ARRAY(wpu, PIC18PortState, PIC18_PORTS),
        VMSTATE_UINT8_ARRAY(odcon, PIC18PortState, PIC18_PORTS),
        VMSTATE_UINT8_ARRAY(slrcon, PIC18PortState, PIC18_PORTS),
        VMSTATE_UINT8_ARRAY(inlvl, PIC18PortState, PIC18_PORTS),
        VMSTATE_UINT8_ARRAY(iocp, PIC18PortState, PIC18_PORTS),
        VMSTATE_UINT8_ARRAY(iocn, PIC18PortState, PIC18_PORTS),
        VMSTATE_UINT8_ARRAY(iocf, PIC18PortState, PIC18_PORTS),
        VMSTATE_UINT8_ARRAY(input, PIC18PortState, PIC18_PORTS),
        VMSTATE_UINT8_ARRAY(pps_mask, PIC18PortState, PIC18_PORTS),
        VMSTATE_UINT8_ARRAY(pps_level, PIC18PortState, PIC18_PORTS),
        VMSTATE_END_OF_LIST()
    }
};

static void pic18_port_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic18_port_realize;
    dc->vmsd = &pic18_port_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = pic18_port_reset_hold;
}

static const TypeInfo pic18_port_types[] = {
    {
        .name = TYPE_PIC18_PORT,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC18PortState),
        .class_init = pic18_port_class_init,
    },
};

DEFINE_TYPES(pic18_port_types)
