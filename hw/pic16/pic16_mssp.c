/*
 * PIC16 MSSP in SPI host and slave modes
 *
 * In host mode (SSPM 0-3, 10), transfers complete immediately, setting BF and
 * pulsing SSPxIF. In slave mode (SSPM 4, 5), writing SSPxBUF preloads the
 * response for the next transfer; incoming external transfers update SSPxBUF,
 * set BF, pulse SSPxIF, and return the preloaded response.
 *
 * An optional chardev connection allows an external client to stream SPI master
 * bytes directly to the slave interface and receive the responses. A byte that
 * arrives while SSPxBUF is still unread is lost and sets SSPOV, as on silicon;
 * the chardev is told to hold its bytes back while BF is set so that a
 * simulated master waits for the slave rather than overrunning it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
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

#define STAT_BF    0    /* buffer full */
#define CON1_SSPOV 6    /* receive overflow */
#define CON1_SSPEN 5    /* module enable */
#define CON1_SSPM  0x0F /* mode select */

uint8_t pic16_mssp_slave_transfer(PIC16MsspState *s, uint8_t in_byte)
{
    uint8_t out_byte = s->tx_buf;

    if (s->stat & (1u << STAT_BF)) {
        /* Receive overflow: the new byte is lost, SSPxBUF keeps the old. */
        s->con1 |= 1u << CON1_SSPOV;
    } else {
        s->buf = in_byte;
        s->stat |= 1u << STAT_BF;
    }
    s->tx_buf = 0xFF;
    qemu_set_irq(s->irq, 1);
    qemu_set_irq(s->irq, 0);

    return out_byte;
}

/* Whether the port answers an outside master rather than driving the bus. */
static bool pic16_mssp_is_slave(PIC16MsspState *s)
{
    return s->external || qemu_chr_fe_backend_connected(&s->chr);
}

static int pic16_mssp_chr_can_receive(void *opaque)
{
    PIC16MsspState *s = opaque;

    return (s->con1 & (1u << CON1_SSPEN)) && !(s->stat & (1u << STAT_BF));
}

static void pic16_mssp_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    PIC16MsspState *s = opaque;

    for (int i = 0; i < size; i++) {
        uint8_t resp = pic16_mssp_slave_transfer(s, buf[i]);
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_write_all(&s->chr, &resp, 1);
        }
    }
}

static uint64_t pic16_mssp_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16MsspState *s = opaque;
    uint8_t value;

    switch (addr) {
    case REG_BUF:
        value = s->buf;
        s->stat &= ~(1u << STAT_BF);
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_accept_input(&s->chr);
        }
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
            s->tx_buf = value;
            s->buf = value;
            break;
        }
        if (pic16_mssp_is_slave(s)) {
            s->tx_buf = value;
            break;
        }
        s->buf = ssi_transfer(s->ssi, value);
        s->stat |= 1u << STAT_BF;
        qemu_set_irq(s->irq, 1);
        qemu_set_irq(s->irq, 0);
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
        if ((s->con1 & (1u << CON1_SSPEN)) &&
            qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_accept_input(&s->chr);
        }
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
    s->tx_buf = 0;
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
    sysbus_init_irq(sbd, &s->irq);

    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_set_handlers(&s->chr, pic16_mssp_chr_can_receive,
                                 pic16_mssp_chr_receive, NULL, NULL, s,
                                 NULL, true);
    }
}

static const Property pic16_mssp_properties[] = {
    DEFINE_PROP_CHR("chardev", PIC16MsspState, chr),
};

static const VMStateDescription pic16_mssp_vmstate = {
    .name = "pic16-mssp",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(buf, PIC16MsspState),
        VMSTATE_UINT8(tx_buf, PIC16MsspState),
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
    device_class_set_props(dc, pic16_mssp_properties);
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
