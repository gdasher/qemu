/*
 * PIC16 EUSART
 *
 * Asynchronous mode only, which is all the firmware uses. Transmission is
 * immediate rather than paced by the baud generator: the divisor is stored and
 * reported but nothing waits for it, so TXxIF is asserted whenever the
 * transmitter is enabled.
 *
 * TXxIF and RCxIF are read-only on hardware -- software clears them by writing
 * TXxREG or reading RCxREG, never by writing PIRx -- so they are driven onto
 * the SoC's level interrupt lines rather than latched.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "pic16_eusart.h"

enum {
    REG_RCREG,
    REG_TXREG,
    REG_BRGL,
    REG_BRGH,
    REG_RCSTA,
    REG_TXSTA,
    REG_BAUDCON,
    PIC16_EUSART_NREGS,
};

/* Bit positions, confirmed against the compiled firmware's UartInit(). */
#define RCSTA_CREN 4
#define RCSTA_SPEN 7
#define RCSTA_OERR 1
#define TXSTA_BRGH 2
#define TXSTA_TXEN 5
#define BAUDCON_BRG16 3

static void pic16_eusart_update_irq(PIC16EusartState *s)
{
    bool enabled = s->rcsta & (1u << RCSTA_SPEN);

    qemu_set_irq(s->tx_irq, enabled && (s->txsta & (1u << TXSTA_TXEN)));
    qemu_set_irq(s->rx_irq, enabled && s->rx_full);
}

static int pic16_eusart_can_receive(void *opaque)
{
    PIC16EusartState *s = opaque;

    return !s->rx_full && (s->rcsta & (1u << RCSTA_CREN));
}

static void pic16_eusart_receive(void *opaque, const uint8_t *buf, int size)
{
    PIC16EusartState *s = opaque;

    if (s->rx_full) {
        /* The receiver has not been drained; flag the overrun as hardware does. */
        s->rcsta |= 1u << RCSTA_OERR;
        return;
    }
    s->rcreg = buf[0];
    s->rx_full = true;
    pic16_eusart_update_irq(s);
}

static uint64_t pic16_eusart_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16EusartState *s = opaque;
    uint8_t value;

    switch (addr) {
    case REG_RCREG:
        value = s->rcreg;
        s->rx_full = false;
        pic16_eusart_update_irq(s);
        return value;
    case REG_TXREG:
        return 0;   /* write-only on hardware */
    case REG_BRGL:
        return s->brgl;
    case REG_BRGH:
        return s->brgh;
    case REG_RCSTA:
        return s->rcsta;
    case REG_TXSTA:
        /*
         * TRMT, the transmit shift register empty flag, sits at bit 1 and is
         * always true here because transmission does not take any time.
         */
        return s->txsta | 0x02;
    case REG_BAUDCON:
        return s->baudcon;
    default:
        return 0;
    }
}

static void pic16_eusart_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    PIC16EusartState *s = opaque;
    uint8_t byte;

    switch (addr) {
    case REG_TXREG:
        byte = value;
        if (s->txsta & (1u << TXSTA_TXEN)) {
            /* Blocking is fine: nothing here can make the guest wait. */
            qemu_chr_fe_write_all(&s->chr, &byte, 1);
        }
        break;
    case REG_BRGL:
        s->brgl = value;
        break;
    case REG_BRGH:
        s->brgh = value;
        break;
    case REG_RCSTA:
        /*
         * Clearing CREN is how firmware acknowledges an overrun, so drop OERR
         * with it. OERR itself is read-only.
         */
        if (!(value & (1u << RCSTA_CREN))) {
            s->rcsta &= ~(1u << RCSTA_OERR);
        }
        s->rcsta = (s->rcsta & (1u << RCSTA_OERR)) |
                   (value & ~(1u << RCSTA_OERR));
        break;
    case REG_TXSTA:
        s->txsta = value;
        break;
    case REG_BAUDCON:
        s->baudcon = value;
        break;
    default:
        break;
    }
    pic16_eusart_update_irq(s);
}

static const MemoryRegionOps pic16_eusart_ops = {
    .read = pic16_eusart_read,
    .write = pic16_eusart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic16_eusart_reset_hold(Object *obj, ResetType type)
{
    PIC16EusartState *s = PIC16_EUSART(obj);

    s->rcreg = 0;
    s->brgl = 0;
    s->brgh = 0;
    s->rcsta = 0;
    s->txsta = 0;
    s->baudcon = 0x08;  /* ABDOVF clear, WUE clear, BRG16 clear, RCIDL set */
    s->rx_full = false;
    pic16_eusart_update_irq(s);
}

static void pic16_eusart_realize(DeviceState *dev, Error **errp)
{
    PIC16EusartState *s = PIC16_EUSART(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &pic16_eusart_ops, s,
                          "pic16.eusart", PIC16_EUSART_NREGS);
    sysbus_init_mmio(sbd, &s->iomem);

    sysbus_init_irq(sbd, &s->tx_irq);
    sysbus_init_irq(sbd, &s->rx_irq);

    qemu_chr_fe_set_handlers(&s->chr, pic16_eusart_can_receive,
                             pic16_eusart_receive, NULL, NULL, s, NULL, true);
}

static const Property pic16_eusart_properties[] = {
    DEFINE_PROP_CHR("chardev", PIC16EusartState, chr),
};

static const VMStateDescription pic16_eusart_vmstate = {
    .name = "pic16-eusart",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(rcreg, PIC16EusartState),
        VMSTATE_UINT8(brgl, PIC16EusartState),
        VMSTATE_UINT8(brgh, PIC16EusartState),
        VMSTATE_UINT8(rcsta, PIC16EusartState),
        VMSTATE_UINT8(txsta, PIC16EusartState),
        VMSTATE_UINT8(baudcon, PIC16EusartState),
        VMSTATE_BOOL(rx_full, PIC16EusartState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic16_eusart_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic16_eusart_realize;
    dc->vmsd = &pic16_eusart_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, pic16_eusart_properties);
    rc->phases.hold = pic16_eusart_reset_hold;
}

static const TypeInfo pic16_eusart_types[] = {
    {
        .name = TYPE_PIC16_EUSART,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC16EusartState),
        .class_init = pic16_eusart_class_init,
    },
};

DEFINE_TYPES(pic16_eusart_types)
