/*
 * PIC32 UART
 *
 * Asynchronous mode, which is all this firmware uses. A character reaches the
 * backend as soon as it is written: nothing here waits out the baud rate,
 * because nothing in the guest can tell the difference. Both status bits it
 * polls -- UTXBF, which says the transmit buffer is full, and TRMT, which says
 * the shifter is empty -- describe a transmitter that has already finished.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/pic32/pic32_regs.h"
#include "hw/pic32/pic32_uart.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

enum {
    R_MODE  = 0x00,
    R_STA   = 0x10,
    R_TXREG = 0x20,
    R_RXREG = 0x30,
    R_BRG   = 0x40,
};

/* UxMODE */
#define MODE_STSEL  (1u << 0)
#define MODE_PDSEL  (3u << 1)
#define MODE_BRGH   (1u << 3)
#define MODE_ON     (1u << 15)

/* UxSTA */
#define STA_URXDA   (1u << 0)
#define STA_OERR    (1u << 1)
#define STA_FERR    (1u << 2)
#define STA_PERR    (1u << 3)
#define STA_RIDLE   (1u << 4)
#define STA_TRMT    (1u << 8)
#define STA_UTXBF   (1u << 9)
#define STA_UTXEN   (1u << 10)
#define STA_UTXBRK  (1u << 11)
#define STA_URXEN   (1u << 12)

/* Bits software may write; the rest are the receiver's and transmitter's. */
#define STA_WMASK   0xFFFFFFF2u

static void pic32_uart_update_irq(PIC32UartState *s)
{
    bool on = s->mode & MODE_ON;

    /*
     * All three lines are level, not latched: the flags they carry are cleared
     * by draining the receiver or filling the transmitter, never by writing
     * the interrupt controller's flag register. Modelling them as edges would
     * leave a handler that only reads UxRXREG re-entering for ever.
     */
    qemu_set_irq(s->irq[PIC32_UART_IRQ_RX], on && (s->sta & STA_URXDA));
    qemu_set_irq(s->irq[PIC32_UART_IRQ_TX],
                 on && (s->sta & STA_UTXEN) && !(s->sta & STA_UTXBF));
    qemu_set_irq(s->irq[PIC32_UART_IRQ_FAULT], on && (s->sta & STA_OERR));
}

static int pic32_uart_can_receive(void *opaque)
{
    PIC32UartState *s = opaque;

    if (!(s->mode & MODE_ON) || !(s->sta & STA_URXEN)) {
        return 0;
    }
    return PIC32_UART_FIFO - s->rx_count;
}

static void pic32_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    PIC32UartState *s = opaque;
    int i;

    for (i = 0; i < size; i++) {
        if (s->rx_count == PIC32_UART_FIFO) {
            /*
             * Hardware drops the character and latches OERR, which stops the
             * receiver until software clears it. Harmony's error handler does
             * exactly that.
             */
            s->sta |= STA_OERR;
            break;
        }
        s->rx[s->rx_count++] = buf[i];
    }
    s->sta |= STA_URXDA;
    pic32_uart_update_irq(s);
}

static uint32_t pic32_uart_read(void *opaque, hwaddr addr)
{
    PIC32UartState *s = opaque;
    uint32_t value;

    switch (addr) {
    case R_MODE:
        return s->mode;
    case R_STA:
        return s->sta;
    case R_TXREG:
        /* Write-only; hardware reads it as zero. */
        return 0;
    case R_RXREG:
        if (!s->rx_count) {
            return 0;
        }
        value = s->rx[0];
        memmove(s->rx, s->rx + 1, --s->rx_count);
        if (!s->rx_count) {
            s->sta &= ~STA_URXDA;
        }
        pic32_uart_update_irq(s);
        qemu_chr_fe_accept_input(&s->chr);
        return value;
    case R_BRG:
        return s->brg;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-uart: read of 0x%02x\n",
                      (unsigned)addr);
        return 0;
    }
}

static void pic32_uart_write(void *opaque, hwaddr addr, uint32_t value)
{
    PIC32UartState *s = opaque;
    uint8_t byte;

    switch (addr) {
    case R_MODE:
        s->mode = value;
        if (!(value & MODE_ON)) {
            s->rx_count = 0;
            s->sta &= ~(STA_URXDA | STA_OERR);
        }
        pic32_uart_update_irq(s);
        break;
    case R_STA:
        /*
         * OERR is cleared by writing zero to it, and clearing it flushes the
         * receiver -- DS61107 21.3. The other read-only bits keep their value.
         */
        if ((s->sta & STA_OERR) && !(value & STA_OERR)) {
            s->rx_count = 0;
            s->sta &= ~STA_URXDA;
            qemu_chr_fe_accept_input(&s->chr);
        }
        s->sta = (s->sta & ~STA_WMASK) | (value & STA_WMASK);
        pic32_uart_update_irq(s);
        break;
    case R_TXREG:
        if (!(s->mode & MODE_ON) || !(s->sta & STA_UTXEN)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pic32-uart: write to UxTXREG with the transmitter "
                          "off\n");
            break;
        }
        byte = value;
        qemu_chr_fe_write_all(&s->chr, &byte, 1);
        pic32_uart_update_irq(s);
        break;
    case R_RXREG:
        break;
    case R_BRG:
        s->brg = value & 0xFFFF;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-uart: write of 0x%08x to 0x%02x\n",
                      value, (unsigned)addr);
        break;
    }
}

static const PIC32RegsOps pic32_uart_regs_ops = {
    .read = pic32_uart_read,
    .write = pic32_uart_write,
};

static void pic32_uart_reset_hold(Object *obj, ResetType type)
{
    PIC32UartState *s = PIC32_UART(obj);

    s->mode = 0;
    /* A transmitter with nothing in it, and an idle receiver. */
    s->sta = STA_TRMT | STA_RIDLE;
    s->brg = 0;
    s->rx_count = 0;
    pic32_uart_update_irq(s);
}

static void pic32_uart_realize(DeviceState *dev, Error **errp)
{
    PIC32UartState *s = PIC32_UART(dev);

    pic32_regs_init_io(&s->mmio, OBJECT(dev), &pic32_uart_regs_ops, s,
                       "pic32-uart", PIC32_UART_SIZE, PIC32_REGS_ALIASED);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    qdev_init_gpio_out_named(dev, s->irq, PIC32_UART_IRQ_GPIO,
                             PIC32_UART_IRQS);

    qemu_chr_fe_set_handlers(&s->chr, pic32_uart_can_receive,
                             pic32_uart_receive, NULL, NULL, s, NULL, true);
}

static const Property pic32_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", PIC32UartState, chr),
};

static const VMStateDescription pic32_uart_vmstate = {
    .name = "pic32-uart",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mode, PIC32UartState),
        VMSTATE_UINT32(sta, PIC32UartState),
        VMSTATE_UINT32(brg, PIC32UartState),
        VMSTATE_UINT8_ARRAY(rx, PIC32UartState, PIC32_UART_FIFO),
        VMSTATE_UINT32(rx_count, PIC32UartState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic32_uart_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic32_uart_realize;
    dc->vmsd = &pic32_uart_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, pic32_uart_properties);
    rc->phases.hold = pic32_uart_reset_hold;
}

static const TypeInfo pic32_uart_types[] = {
    {
        .name = TYPE_PIC32_UART,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC32UartState),
        .class_init = pic32_uart_class_init,
    },
};

DEFINE_TYPES(pic32_uart_types)
