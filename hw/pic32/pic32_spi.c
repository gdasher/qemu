/*
 * PIC32 SPI controller, host mode
 *
 * A transfer happens the moment the guest writes SPIxBUF: the byte goes out on
 * the SSI bus and what comes back lands in the receive FIFO. Nothing waits out
 * the baud rate, because the only thing that could tell the difference is a
 * device that cares how long a clock takes, and none of the ones modelled here
 * do.
 *
 * The receive FIFO is the part that has to be right. Harmony's driver writes
 * sixteen bytes into SPIxBUF and then spins until RXBUFELM says sixteen have
 * come back, so a controller that took the writes and left the count at zero
 * would hang the guest on its first SD card command -- and that is the first
 * thing this firmware does after it starts its scheduler.
 *
 * Chip select is not modelled here on purpose. The firmware drives it as an
 * ordinary GPIO -- MSSEN is 0 in every controller it configures -- so the
 * board wires a port pin to each device's select line, and this controller
 * only ever moves data.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/pic32/pic32_regs.h"
#include "hw/pic32/pic32_spi.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "hw/core/qdev-clock.h"

enum {
    R_CON  = 0x00,
    R_STAT = 0x10,
    R_BUF  = 0x20,
    R_BRG  = 0x30,
    R_CON2 = 0x40,
};

/* SPIxCON */
#define CON_MSTEN   (1u << 5)
#define CON_CKP     (1u << 6)
#define CON_CKE     (1u << 8)
#define CON_MODE16  (1u << 10)
#define CON_MODE32  (1u << 11)
#define CON_ON      (1u << 15)
#define CON_ENHBUF  (1u << 16)

/* SPIxSTAT */
#define STAT_SPIRBF (1u << 0)
#define STAT_SPITBF (1u << 1)
#define STAT_SPITBE (1u << 3)
#define STAT_SPIRBE (1u << 5)
#define STAT_SPIROV (1u << 6)
#define STAT_SRMT   (1u << 7)
#define STAT_SPITUR (1u << 8)
#define STAT_SPIBUSY (1u << 11)
#define STAT_TXBUFELM_SHIFT 16
#define STAT_RXBUFELM_SHIFT 24
#define STAT_ELM_MASK 0x1F

/* Only SPIROV is software's to clear; the rest describe the FIFOs. */
#define STAT_WMASK STAT_SPIROV

static unsigned pic32_spi_width(PIC32SpiState *s)
{
    if (s->con & CON_MODE32) {
        return 32;
    }
    return (s->con & CON_MODE16) ? 16 : 8;
}

/*
 * Everything in SPIxSTAT except SPIROV is a function of how full the FIFOs
 * are, so it is computed on the way out rather than tracked. The transmit side
 * is always empty and the shifter always idle: by the time the guest can look,
 * the transfer it asked for has already happened.
 */
static uint32_t pic32_spi_stat(PIC32SpiState *s)
{
    uint32_t stat = s->stat & STAT_SPIROV;

    if (s->serial_out) {
        if (!s->tx_count) {
            stat |= STAT_SPITBE;
        }
        if (s->tx_count == PIC32_SPI_FIFO) {
            stat |= STAT_SPITBF;
        }
        if (s->shift_bits || timer_pending(s->shift)) {
            stat |= STAT_SPIBUSY;
        } else {
            stat |= STAT_SRMT;
        }
        stat |= (s->tx_count & STAT_ELM_MASK) << STAT_TXBUFELM_SHIFT;
        return stat;
    }

    stat |= STAT_SPITBE | STAT_SRMT;
    if (s->rx_count) {
        stat |= STAT_SPIRBF;
    } else {
        stat |= STAT_SPIRBE;
    }
    if (s->rx_count == PIC32_SPI_FIFO) {
        stat |= STAT_SPITBF;
    }
    stat |= (s->rx_count & STAT_ELM_MASK) << STAT_RXBUFELM_SHIFT;
    return stat;
}

static void pic32_spi_update_irq(PIC32SpiState *s)
{
    bool on = s->con & CON_ON;

    qemu_set_irq(s->irq[PIC32_SPI_IRQ_RX], on && s->rx_count != 0);
    if (!s->serial_out) {
        qemu_set_irq(s->irq[PIC32_SPI_IRQ_TX], on);
    }
    qemu_set_irq(s->irq[PIC32_SPI_IRQ_FAULT], on && (s->stat & STAT_SPIROV));
}

/*
 * How long one bit takes on the wire. The baud generator halves the peripheral
 * clock and then divides by BRG+1, which is the only number here that has to
 * be right: a strip on the other end of the pin decides what a bit means from
 * how long the line was high.
 */
static int64_t pic32_spi_bit_ns(PIC32SpiState *s)
{
    uint64_t hz = clock_get_hz(s->pbclk);
    uint64_t baud = hz / (2 * ((s->brg & 0x1FF) + 1));

    return baud ? (int64_t)(NANOSECONDS_PER_SECOND / baud) : 0;
}

static void pic32_spi_sdo(PIC32SpiState *s, bool level)
{
    if (level != s->sdo_level) {
        s->sdo_level = level;
        qemu_set_irq(s->sdo, level);
    }
}

static void pic32_spi_shift_start(PIC32SpiState *s);

/*
 * One bit has finished. The line carries the next one, and a word running out
 * frees a place in the transmit buffer -- which is what the transfer-done
 * source reports, and so what a DMA channel feeding this controller waits for.
 */
static void pic32_spi_shift_tick(void *opaque)
{
    PIC32SpiState *s = opaque;

    if (s->shift_bits) {
        s->shift_bits--;
        pic32_spi_sdo(s, (s->shift_reg >> s->shift_bits) & 1);
        timer_mod(s->shift, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            pic32_spi_bit_ns(s));
        return;
    }

    /* The word is out. Tell anyone waiting, then take the next one. */
    qemu_irq_pulse(s->irq[PIC32_SPI_IRQ_TX]);
    pic32_spi_shift_start(s);
}

static void pic32_spi_shift_start(PIC32SpiState *s)
{
    unsigned width;

    if (!s->tx_count) {
        return;                 /* idle, holding the last bit sent */
    }

    width = pic32_spi_width(s);
    s->shift_reg = s->tx[0];
    memmove(s->tx, s->tx + 1, --s->tx_count * sizeof(s->tx[0]));
    s->shift_bits = width - 1;
    pic32_spi_sdo(s, (s->shift_reg >> s->shift_bits) & 1);
    timer_mod(s->shift, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                        pic32_spi_bit_ns(s));
}

static void pic32_spi_transfer(PIC32SpiState *s, uint32_t value)
{
    unsigned width = pic32_spi_width(s);
    uint32_t in = 0;
    unsigned i;

    if (s->serial_out) {
        /*
         * Nothing on the far end of a pin answers back, so there is no receive
         * side here: the word joins the queue for the shifter and the guest
         * finds out it has gone when the transfer-done source fires.
         */
        if (s->tx_count == PIC32_SPI_FIFO) {
            s->stat |= STAT_SPITUR;
            return;
        }
        s->tx[s->tx_count++] = value;
        if (!s->shift_bits && !timer_pending(s->shift)) {
            pic32_spi_shift_start(s);
        }
        return;
    }

    /*
     * SSI moves a byte at a time, so a wider transfer is that many bytes, most
     * significant first -- which is the order the shift register sends them.
     */
    for (i = width; i; i -= 8) {
        in = (in << 8) | ssi_transfer(s->ssi, (value >> (i - 8)) & 0xFF);
    }

    if (s->rx_count == PIC32_SPI_FIFO) {
        /*
         * Hardware discards the new data and latches the overrun, which stops
         * nothing: the guest keeps clocking and finds out when it reads STAT.
         */
        s->stat |= STAT_SPIROV;
        return;
    }
    s->rx[s->rx_count++] = in;
}

static uint32_t pic32_spi_read(void *opaque, hwaddr addr)
{
    PIC32SpiState *s = opaque;
    uint32_t value;

    switch (addr) {
    case R_CON:
        return s->con;
    case R_STAT:
        return pic32_spi_stat(s);
    case R_BUF:
        if (!s->rx_count) {
            /* Hardware gives back whatever the buffer last held. */
            return 0;
        }
        value = s->rx[0];
        memmove(s->rx, s->rx + 1, --s->rx_count * sizeof(s->rx[0]));
        pic32_spi_update_irq(s);
        return value;
    case R_BRG:
        return s->brg;
    case R_CON2:
        return s->con2;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-spi: read of 0x%02x\n",
                      (unsigned)addr);
        return 0;
    }
}

static void pic32_spi_write(void *opaque, hwaddr addr, uint32_t value)
{
    PIC32SpiState *s = opaque;

    switch (addr) {
    case R_CON:
        s->con = value;
        if (!(value & CON_ON)) {
            s->rx_count = 0;
            s->stat = 0;
        } else if (!(value & CON_MSTEN)) {
            qemu_log_mask(LOG_UNIMP,
                          "pic32-spi: client mode is not modelled\n");
        }
        pic32_spi_update_irq(s);
        break;
    case R_STAT:
        s->stat = value & STAT_WMASK;
        pic32_spi_update_irq(s);
        break;
    case R_BUF:
        if (!(s->con & CON_ON)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pic32-spi: write to SPIxBUF with the module off\n");
            break;
        }
        pic32_spi_transfer(s, value);
        pic32_spi_update_irq(s);
        break;
    case R_BRG:
        s->brg = value & 0x1FFF;
        break;
    case R_CON2:
        s->con2 = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-spi: write of 0x%08x to 0x%02x\n",
                      value, (unsigned)addr);
        break;
    }
}

static const PIC32RegsOps pic32_spi_regs_ops = {
    .read = pic32_spi_read,
    .write = pic32_spi_write,
};

static void pic32_spi_reset_hold(Object *obj, ResetType type)
{
    PIC32SpiState *s = PIC32_SPI(obj);

    s->con = 0;
    s->con2 = 0;
    s->stat = 0;
    s->brg = 0;
    s->rx_count = 0;
    s->tx_count = 0;
    s->shift_bits = 0;
    if (s->shift) {
        timer_del(s->shift);
    }
    pic32_spi_update_irq(s);
}

static void pic32_spi_realize(DeviceState *dev, Error **errp)
{
    PIC32SpiState *s = PIC32_SPI(dev);

    if (s->serial_out && !clock_has_source(s->pbclk)) {
        error_setg(errp, "pic32-spi: no peripheral clock to time bits with");
        return;
    }

    s->ssi = ssi_create_bus(dev, "ssi");
    s->shift = timer_new_ns(QEMU_CLOCK_VIRTUAL, pic32_spi_shift_tick, s);

    pic32_regs_init_io(&s->mmio, OBJECT(dev), &pic32_spi_regs_ops, s,
                       "pic32-spi", PIC32_SPI_SIZE, PIC32_REGS_ALIASED);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    qdev_init_gpio_out_named(dev, s->irq, PIC32_SPI_IRQ_GPIO, PIC32_SPI_IRQS);
    qdev_init_gpio_out_named(dev, &s->sdo, PIC32_SPI_SDO_GPIO, 1);
}

static const Property pic32_spi_properties[] = {
    /*
     * Set for a controller whose SDO goes to a port pin rather than to a
     * device on the bus. It changes what a write to SPIxBUF means: the word
     * is shifted out a bit at a time at the baud rate instead of being handed
     * to whatever is on the other end all at once.
     */
    DEFINE_PROP_BOOL("serial-out", PIC32SpiState, serial_out, false),
};

static const VMStateDescription pic32_spi_vmstate = {
    .name = "pic32-spi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(con, PIC32SpiState),
        VMSTATE_UINT32(con2, PIC32SpiState),
        VMSTATE_UINT32(stat, PIC32SpiState),
        VMSTATE_UINT32(brg, PIC32SpiState),
        VMSTATE_UINT32_ARRAY(rx, PIC32SpiState, PIC32_SPI_FIFO),
        VMSTATE_UINT32(rx_count, PIC32SpiState),
        VMSTATE_UINT32_ARRAY(tx, PIC32SpiState, PIC32_SPI_FIFO),
        VMSTATE_UINT32(tx_count, PIC32SpiState),
        VMSTATE_UINT32(shift_reg, PIC32SpiState),
        VMSTATE_UINT32(shift_bits, PIC32SpiState),
        VMSTATE_BOOL(sdo_level, PIC32SpiState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic32_spi_init(Object *obj)
{
    PIC32SpiState *s = PIC32_SPI(obj);

    s->pbclk = qdev_init_clock_in(DEVICE(obj), "pbclk", NULL, NULL, 0);
}

static void pic32_spi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic32_spi_realize;
    dc->vmsd = &pic32_spi_vmstate;
    device_class_set_props(dc, pic32_spi_properties);
    dc->user_creatable = false;
    rc->phases.hold = pic32_spi_reset_hold;
}

static const TypeInfo pic32_spi_types[] = {
    {
        .name = TYPE_PIC32_SPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC32SpiState),
        .instance_init = pic32_spi_init,
        .class_init = pic32_spi_class_init,
    },
};

DEFINE_TYPES(pic32_spi_types)
