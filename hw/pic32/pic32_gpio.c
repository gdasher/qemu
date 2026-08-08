/*
 * PIC32 I/O ports
 *
 * One device for ports A to G, because they are one contiguous run of register
 * space with 0x100 to a port and the same ten registers in each.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/pic32/pic32_gpio.h"
#include "hw/pic32/pic32_regs.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"

/* Register offsets within a port's 0x100. */
enum {
    R_ANSEL  = 0x00,
    R_TRIS   = 0x10,
    R_PORT   = 0x20,
    R_LAT    = 0x30,
    R_ODC    = 0x40,
    R_CNPU   = 0x50,
    R_CNPD   = 0x60,
    R_CNCON  = 0x70,
    R_CNEN   = 0x80,
    R_CNSTAT = 0x90,
    R_CNNE   = 0xA0,
    R_CNF    = 0xB0,
    R_SRCON0 = 0xC0,
    R_SRCON1 = 0xD0,
};

#define PIN_MASK ((1u << PIC32_GPIO_PINS) - 1)

/*
 * Reading a port gives the levels on the pins: what the latch drives where the
 * pin is an output, what the outside world drives where it is not. TRIS is 1
 * for an input, the opposite sense to most parts and the same as every other
 * PIC.
 */
static uint32_t pic32_gpio_pins(PIC32GpioState *s, unsigned p)
{
    return (s->lat[p] & ~s->tris[p]) | (s->input[p] & s->tris[p]);
}

static void pic32_gpio_update_outputs(PIC32GpioState *s, unsigned p)
{
    unsigned pin;

    for (pin = 0; pin < PIC32_GPIO_PINS; pin++) {
        if (!(s->tris[p] & (1u << pin))) {
            qemu_set_irq(s->out[p * PIC32_GPIO_PINS + pin],
                         (s->lat[p] >> pin) & 1);
        }
    }
}

static void pic32_gpio_set_pin(void *opaque, int line, int level)
{
    PIC32GpioState *s = opaque;
    unsigned p = line / PIC32_GPIO_PINS;
    uint32_t mask = 1u << (line % PIC32_GPIO_PINS);

    if (level) {
        s->input[p] |= mask;
    } else {
        s->input[p] &= ~mask;
    }
}

/*
 * "input-a" and friends expose what the outside world drives onto a port, so a
 * test can move a pin over QMP with no device in between:
 *
 *   qom-set /machine/soc/gpio input-d 0x100
 */
static void pic32_gpio_get_input(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    PIC32GpioState *s = PIC32_GPIO(obj);
    uint32_t value = s->input[(uintptr_t)opaque];

    visit_type_uint32(v, name, &value, errp);
}

static void pic32_gpio_set_input(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    PIC32GpioState *s = PIC32_GPIO(obj);
    unsigned p = (uintptr_t)opaque;
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    s->input[p] = value & PIN_MASK;
}

static uint32_t pic32_gpio_read(void *opaque, hwaddr addr)
{
    PIC32GpioState *s = opaque;
    unsigned p = addr / PIC32_GPIO_PORT_SIZE;

    switch (addr % PIC32_GPIO_PORT_SIZE) {
    case R_ANSEL:
        return s->ansel[p];
    case R_TRIS:
        return s->tris[p];
    case R_PORT:
        return pic32_gpio_pins(s, p);
    case R_LAT:
        return s->lat[p];
    case R_ODC:
        return s->odc[p];
    case R_CNPU:
        return s->cnpu[p];
    case R_CNPD:
        return s->cnpd[p];
    case R_CNCON:
        return s->cncon[p];
    case R_CNEN:
        return s->cnen[p];
    case R_CNSTAT:
        return s->cnstat[p];
    case R_CNNE:
        return s->cnne[p];
    case R_CNF:
        return s->cnf[p];
    case R_SRCON0:
    case R_SRCON1:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-gpio: read of port %c + 0x%02x\n",
                      'A' + p, (unsigned)(addr % PIC32_GPIO_PORT_SIZE));
        return 0;
    }
}

static void pic32_gpio_write(void *opaque, hwaddr addr, uint32_t value)
{
    PIC32GpioState *s = opaque;
    unsigned p = addr / PIC32_GPIO_PORT_SIZE;

    value &= PIN_MASK;

    switch (addr % PIC32_GPIO_PORT_SIZE) {
    case R_ANSEL:
        s->ansel[p] = value;
        break;
    case R_TRIS:
        s->tris[p] = value;
        pic32_gpio_update_outputs(s, p);
        break;
    /*
     * Writing PORTx writes the latch. The data sheet describes it that way
     * too, and firmware relies on it: GPIO_PinWrite() goes through LATx, but
     * plenty of hand-written code does not.
     */
    case R_PORT:
    case R_LAT:
        s->lat[p] = value;
        pic32_gpio_update_outputs(s, p);
        break;
    case R_ODC:
        s->odc[p] = value;
        break;
    case R_CNPU:
        s->cnpu[p] = value;
        break;
    case R_CNPD:
        s->cnpd[p] = value;
        break;
    case R_CNCON:
        s->cncon[p] = value;
        break;
    case R_CNEN:
        s->cnen[p] = value;
        break;
    case R_CNNE:
        s->cnne[p] = value;
        break;
    /* CNSTAT and CNF are set by the hardware; a write to either is ignored. */
    case R_CNSTAT:
    case R_CNF:
    case R_SRCON0:
    case R_SRCON1:
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "pic32-gpio: write of 0x%08x to port %c + 0x%02x\n",
                      value, 'A' + p,
                      (unsigned)(addr % PIC32_GPIO_PORT_SIZE));
        break;
    }
}

static const PIC32RegsOps pic32_gpio_regs_ops = {
    .read = pic32_gpio_read,
    .write = pic32_gpio_write,
};

static void pic32_gpio_reset_hold(Object *obj, ResetType type)
{
    PIC32GpioState *s = PIC32_GPIO(obj);
    unsigned p;

    for (p = 0; p < PIC32_GPIO_PORTS; p++) {
        /* Every pin comes up an analogue input, so nothing is driven. */
        s->ansel[p] = PIN_MASK;
        s->tris[p] = PIN_MASK;
        s->lat[p] = 0;
        s->odc[p] = 0;
        s->cnpu[p] = 0;
        s->cnpd[p] = 0;
        s->cncon[p] = 0;
        s->cnen[p] = 0;
        s->cnne[p] = 0;
        s->cnstat[p] = 0;
        s->cnf[p] = 0;
    }
}

static void pic32_gpio_realize(DeviceState *dev, Error **errp)
{
    PIC32GpioState *s = PIC32_GPIO(dev);

    pic32_regs_init_io(&s->mmio, OBJECT(dev), &pic32_gpio_regs_ops, s,
                       "pic32-gpio", PIC32_GPIO_SIZE, PIC32_REGS_ALIASED);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    qdev_init_gpio_out_named(dev, s->out, PIC32_GPIO_OUT_GPIO,
                             PIC32_GPIO_LINES);
    qdev_init_gpio_in_named(dev, pic32_gpio_set_pin, PIC32_GPIO_IN_GPIO,
                            PIC32_GPIO_LINES);
}

static void pic32_gpio_init(Object *obj)
{
    unsigned p;

    for (p = 0; p < PIC32_GPIO_PORTS; p++) {
        g_autofree char *name = g_strdup_printf("input-%c", 'a' + p);

        object_property_add(obj, name, "uint32", pic32_gpio_get_input,
                            pic32_gpio_set_input, NULL, (void *)(uintptr_t)p);
    }
}

static const VMStateDescription pic32_gpio_vmstate = {
    .name = "pic32-gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(ansel, PIC32GpioState, PIC32_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(tris, PIC32GpioState, PIC32_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(lat, PIC32GpioState, PIC32_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(odc, PIC32GpioState, PIC32_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(cnpu, PIC32GpioState, PIC32_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(cnpd, PIC32GpioState, PIC32_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(cncon, PIC32GpioState, PIC32_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(cnen, PIC32GpioState, PIC32_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(cnne, PIC32GpioState, PIC32_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(cnstat, PIC32GpioState, PIC32_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(cnf, PIC32GpioState, PIC32_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(input, PIC32GpioState, PIC32_GPIO_PORTS),
        VMSTATE_END_OF_LIST()
    }
};

static void pic32_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic32_gpio_realize;
    dc->vmsd = &pic32_gpio_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = pic32_gpio_reset_hold;
}

static const TypeInfo pic32_gpio_types[] = {
    {
        .name = TYPE_PIC32_GPIO,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC32GpioState),
        .instance_init = pic32_gpio_init,
        .class_init = pic32_gpio_class_init,
    },
};

DEFINE_TYPES(pic32_gpio_types)
