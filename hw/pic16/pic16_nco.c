/*
 * PIC16 Numerically Controlled Oscillator (NCO)
 *
 * Models the 20-bit increment and accumulator registers. The increment is
 * double-buffered: NCO1INCU and NCO1INCH must be written first, and the write
 * to NCO1INCL loads all three into the active increment (data sheet 22.1.4).
 * On silicon that load lands on the second NCO clock edge after the INCL
 * write, which at any usable NCO clock is before the next instruction can
 * complete, so it is immediate here. Writing INCL first, as a firmware might
 * be tempted to, therefore loads the new low byte with the *old* high bytes,
 * exactly as the chip does.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "pic16_nco.h"

#define NCO_CON_N1EN  7
#define NCO_CON_N1OUT 6
#define NCO_CON_N1POL 5
#define NCO_CON_N1PFM 0

static uint64_t pic16_nco_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16NcoState *s = opaque;

    switch (addr) {
    case REG_NCO_ACCL:
        return s->acc & 0xFF;
    case REG_NCO_ACCH:
        return (s->acc >> 8) & 0xFF;
    case REG_NCO_ACCU:
        return (s->acc >> 16) & 0x0F;
    case REG_NCO_INCL:
        return s->inc_buf[0];
    case REG_NCO_INCH:
        return s->inc_buf[1];
    case REG_NCO_INCU:
        return s->inc_buf[2] & 0x0F;
    case REG_NCO_CON:
        /* Output bit N1OUT is set when module is enabled and running */
        return s->con | ((s->con & (1u << NCO_CON_N1EN)) ? (1u << NCO_CON_N1OUT) : 0);
    case REG_NCO_CLK:
        return s->clk;
    default:
        return 0;
    }
}

static void pic16_nco_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    PIC16NcoState *s = opaque;

    switch (addr) {
    case REG_NCO_ACCL:
        s->acc = (s->acc & 0xFFF00) | (value & 0xFF);
        break;
    case REG_NCO_ACCH:
        s->acc = (s->acc & 0xF00FF) | ((value & 0xFF) << 8);
        break;
    case REG_NCO_ACCU:
        s->acc = (s->acc & 0x0FFFF) | ((value & 0x0F) << 16);
        break;
    case REG_NCO_INCL:
        /* The low byte goes last: this write loads the increment buffer. */
        s->inc_buf[0] = value;
        s->inc = s->inc_buf[0] | ((uint32_t)s->inc_buf[1] << 8) |
                 ((uint32_t)s->inc_buf[2] << 16);
        if (s->inc_sink) {
            s->inc_sink(s->inc_sink_opaque, s->inc);
        }
        break;
    case REG_NCO_INCH:
        s->inc_buf[1] = value;
        break;
    case REG_NCO_INCU:
        s->inc_buf[2] = value & 0x0F;
        break;
    case REG_NCO_CON:
        s->con = value;
        break;
    case REG_NCO_CLK:
        s->clk = value;
        break;
    default:
        break;
    }
}

void pic16_nco_set_increment_sink(PIC16NcoState *s,
                                  void (*sink)(void *opaque, uint32_t inc),
                                  void *opaque)
{
    s->inc_sink = sink;
    s->inc_sink_opaque = opaque;
}

static const MemoryRegionOps pic16_nco_ops = {
    .read = pic16_nco_read,
    .write = pic16_nco_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic16_nco_get_prop(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    PIC16NcoState *s = PIC16_NCO(obj);
    uint32_t val = (uintptr_t)opaque == 1 ? s->inc : s->acc;
    visit_type_uint32(v, name, &val, errp);
}

static void pic16_nco_reset_hold(Object *obj, ResetType type)
{
    PIC16NcoState *s = PIC16_NCO(obj);

    s->acc = 0;
    s->inc = 0x00001;
    memset(s->inc_buf, 0, sizeof(s->inc_buf));
    s->inc_buf[0] = 0x01;
    s->con = 0;
    s->clk = 0;
}

static void pic16_nco_realize(DeviceState *dev, Error **errp)
{
    PIC16NcoState *s = PIC16_NCO(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &pic16_nco_ops, s,
                          "pic16.nco", PIC16_NCO_NREGS);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out(dev, &s->out, 1);

    object_property_add(OBJECT(dev), "increment", "uint32",
                        pic16_nco_get_prop, NULL, NULL, (void *)(uintptr_t)1);
    object_property_add(OBJECT(dev), "accumulator", "uint32",
                        pic16_nco_get_prop, NULL, NULL, (void *)(uintptr_t)2);
}

static const VMStateDescription pic16_nco_vmstate = {
    .name = "pic16-nco",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(acc, PIC16NcoState),
        VMSTATE_UINT32(inc, PIC16NcoState),
        VMSTATE_UINT8_ARRAY(inc_buf, PIC16NcoState, 3),
        VMSTATE_UINT8(con, PIC16NcoState),
        VMSTATE_UINT8(clk, PIC16NcoState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic16_nco_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic16_nco_realize;
    dc->vmsd = &pic16_nco_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = pic16_nco_reset_hold;
}

static const TypeInfo pic16_nco_types[] = {
    {
        .name = TYPE_PIC16_NCO,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC16NcoState),
        .class_init = pic16_nco_class_init,
    },
};

DEFINE_TYPES(pic16_nco_types)
