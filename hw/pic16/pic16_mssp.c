/*
 * PIC16 MSSP in SPI host mode
 *
 * Only host mode is modelled; I2C is not. Transfers complete immediately, so
 * BF is set the moment SSPxBUF is written and cleared when it is read back,
 * which is the handshake firmware polls.
 *
 * With nothing attached to the bus a transfer reads back zero. That happens to
 * be the safe value for the reference firmware, whose sensor expander reports
 * "no limit tripped" as zero -- 0xFF would latch a fault on every move. Do not
 * change it to something more "obviously invalid".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "pic16_mssp.h"

enum {
    REG_BUF,
    REG_ADD,
    REG_MSK,
    REG_STAT,
    REG_CON1,
    REG_CON2,
    REG_CON3,
    PIC16_MSSP_NREGS,
};

#define STAT_BF   0     /* buffer full */
#define CON1_SSPEN 5    /* module enable */
#define CON1_SSPM  0x0F /* mode select; 0-5 are the host SPI modes */

static uint64_t pic16_mssp_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16MsspState *s = opaque;
    uint8_t value;

    switch (addr) {
    case REG_BUF:
        value = s->buf;
        s->stat &= ~(1u << STAT_BF);
        return value;
    case REG_ADD:
        return s->add;
    case REG_MSK:
        return s->msk;
    case REG_STAT:
        return s->stat;
    case REG_CON1:
        return s->con1;
    case REG_CON2:
        return s->con2;
    case REG_CON3:
        return s->con3;
    default:
        return 0;
    }
}

static void pic16_mssp_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    PIC16MsspState *s = opaque;

    switch (addr) {
    case REG_BUF:
        if (!(s->con1 & (1u << CON1_SSPEN))) {
            break;
        }
        if ((s->con1 & CON1_SSPM) > 5) {
            qemu_log_mask(LOG_UNIMP,
                          "pic16-mssp: mode %u is not host SPI; "
                          "only host mode is modelled\n",
                          s->con1 & CON1_SSPM);
            break;
        }
        s->buf = ssi_transfer(s->ssi, value);
        s->stat |= 1u << STAT_BF;
        break;
    case REG_ADD:
        s->add = value;
        break;
    case REG_MSK:
        s->msk = value;
        break;
    case REG_STAT:
        /* BF is read-only. */
        s->stat = (s->stat & (1u << STAT_BF)) | (value & ~(1u << STAT_BF));
        break;
    case REG_CON1:
        s->con1 = value;
        break;
    case REG_CON2:
        s->con2 = value;
        break;
    case REG_CON3:
        s->con3 = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pic16_mssp_ops = {
    .read = pic16_mssp_read,
    .write = pic16_mssp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic16_mssp_reset_hold(Object *obj, ResetType type)
{
    PIC16MsspState *s = PIC16_MSSP(obj);

    s->buf = 0;
    s->add = 0;
    s->msk = 0;
    s->stat = 0;
    s->con1 = 0;
    s->con2 = 0;
    s->con3 = 0;
}

static void pic16_mssp_realize(DeviceState *dev, Error **errp)
{
    PIC16MsspState *s = PIC16_MSSP(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->ssi = ssi_create_bus(dev, "ssi");

    memory_region_init_io(&s->iomem, OBJECT(dev), &pic16_mssp_ops, s,
                          "pic16.mssp", PIC16_MSSP_NREGS);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription pic16_mssp_vmstate = {
    .name = "pic16-mssp",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(buf, PIC16MsspState),
        VMSTATE_UINT8(add, PIC16MsspState),
        VMSTATE_UINT8(msk, PIC16MsspState),
        VMSTATE_UINT8(stat, PIC16MsspState),
        VMSTATE_UINT8(con1, PIC16MsspState),
        VMSTATE_UINT8(con2, PIC16MsspState),
        VMSTATE_UINT8(con3, PIC16MsspState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic16_mssp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic16_mssp_realize;
    dc->vmsd = &pic16_mssp_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = pic16_mssp_reset_hold;
}

static const TypeInfo pic16_mssp_types[] = {
    {
        .name = TYPE_PIC16_MSSP,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC16MsspState),
        .class_init = pic16_mssp_class_init,
    },
};

DEFINE_TYPES(pic16_mssp_types)
