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
 * thing this firmware does after it starts its scheduler. The FIFO is 128
 * bits of storage, so it holds sixteen bytes, eight half-words or four words
 * depending on MODE<32,16>, and filling it past that raises SPIROV just as
 * the silicon would.
 *
 * A serial-out controller raises its transmit event once per word, at the
 * moment the word finishes on the wire. That is when hardware raises it with
 * STXISEL set to "transmit complete" (00) or "buffer empty" (01, give or
 * take one word of phase); the other two settings assert on available buffer
 * space, which would let a DMA channel run a whole FIFO ahead of the wire,
 * and this model does not queue that deep ahead of the shifter -- so those
 * settings get a one-shot LOG_UNIMP instead of quietly behaving like 01.
 *
 * A bus-timed controller has a device on the SSI bus like the default kind,
 * but hands each word over only when the shifter would have finished it:
 * the word waits in the transmit FIFO, the shift timer runs for the word's
 * time at the baud rate, and the exchange with the peripheral -- and the
 * transmit event a DMA channel steps on -- happen at the end. That is the
 * mode for a peripheral whose own timing matters (a microcontroller slave
 * that has to keep up byte by byte), and it is what lets a DMA channel feed
 * the controller without recursing through its own trigger.
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
#define CON_SRXISEL(con) ((con) & 3u)
#define CON_STXISEL(con) (((con) >> 2) & 3u)
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

/* SPIxCON2 */
#define CON2_SPITUREN (1u << 10)
#define CON2_SPIROVEN (1u << 11)

/* SPIxBRG is BRG<12:0>. */
#define BRG_MASK 0x1FFF

static unsigned pic32_spi_width(PIC32SpiState *s)
{
    if (s->con & CON_MODE32) {
        return 32;
    }
    return (s->con & CON_MODE16) ? 16 : 8;
}

/*
 * The FIFO is 128 bits of storage, so its depth in elements follows from the
 * word width. Standard buffer mode keeps the same backing store rather than
 * silicon's single buffer register pair; the drivers that run without ENHBUF
 * exchange one word at a time and never see the difference.
 */
static unsigned pic32_spi_fifo_depth(PIC32SpiState *s)
{
    return 128 / pic32_spi_width(s);
}

/*
 * Everything in SPIxSTAT except SPIROV is a function of how full the FIFOs
 * are, so it is computed on the way out rather than tracked. On a bus-path
 * controller the transmit side is always empty and the shifter always idle:
 * by the time the guest can look, the transfer it asked for has already
 * happened.
 */
static uint32_t pic32_spi_stat(PIC32SpiState *s)
{
    bool enhbuf = s->con & CON_ENHBUF;
    unsigned depth = pic32_spi_fifo_depth(s);
    uint32_t stat = s->stat & STAT_SPIROV;

    if (s->serial_out || s->bus_timed) {
        if (!s->tx_count) {
            stat |= STAT_SPITBE;
        }
        if (s->tx_count == depth) {
            stat |= STAT_SPITBF;
        }
        if (s->shift_bits || timer_pending(s->shift)) {
            stat |= STAT_SPIBUSY;
        } else if (enhbuf) {
            stat |= STAT_SRMT;
        }
        if (enhbuf) {
            stat |= (s->tx_count & STAT_ELM_MASK) << STAT_TXBUFELM_SHIFT;
        }
        if (s->serial_out) {
            if (enhbuf) {
                /* Nothing on the far end of a pin ever answers back. */
                stat |= STAT_SPIRBE;
            }
            return stat;
        }
    } else {
        stat |= STAT_SPITBE;
    }

    if (enhbuf) {
        /* SRMT, SPIRBE and the element counts only mean anything here. */
        if (!s->bus_timed) {
            stat |= STAT_SRMT;
        }
        if (s->rx_count == depth) {
            stat |= STAT_SPIRBF;
        }
        if (!s->rx_count) {
            stat |= STAT_SPIRBE;
        }
        stat |= (s->rx_count & STAT_ELM_MASK) << STAT_RXBUFELM_SHIFT;
    } else if (s->rx_count) {
        /* In standard buffer mode SPIRBF just means data is waiting. */
        stat |= STAT_SPIRBF;
    }
    return stat;
}

/*
 * When the receive source asserts depends on SRXISEL. The reset value asks
 * for an event when the last word is read back out, which is the one edge a
 * model that computes state on the way out cannot see coming; "not empty" is
 * the nearest condition, and more eager rather than less.
 */
static bool pic32_spi_rx_irq_level(PIC32SpiState *s)
{
    unsigned depth = pic32_spi_fifo_depth(s);

    switch (CON_SRXISEL(s->con)) {
    case 2:
        return s->rx_count >= depth / 2;
    case 3:
        return s->rx_count == depth;
    case 0:
        if (s->rx_count && !s->srxisel_logged) {
            s->srxisel_logged = true;
            qemu_log_mask(LOG_UNIMP,
                          "pic32-spi: SRXISEL=0 (buffer emptied by read) is "
                          "treated as SRXISEL=1 (buffer not empty)\n");
        }
        /* fall through */
    default:
        return s->rx_count != 0;
    }
}

static void pic32_spi_update_irq(PIC32SpiState *s)
{
    bool on = s->con & CON_ON;

    qemu_set_irq(s->irq[PIC32_SPI_IRQ_RX], on && pic32_spi_rx_irq_level(s));
    if (!s->serial_out && !s->bus_timed) {
        qemu_set_irq(s->irq[PIC32_SPI_IRQ_TX], on);
    }
    qemu_set_irq(s->irq[PIC32_SPI_IRQ_FAULT],
                 on && (s->con2 & CON2_SPIROVEN) && (s->stat & STAT_SPIROV));
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
    uint64_t baud = hz / (2 * ((s->brg & BRG_MASK) + 1));

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
 * One word crosses the SSI bus. SSI moves a byte at a time, so a wider
 * transfer is that many bytes, most significant first -- which is the order
 * the shift register sends them.
 */
static void pic32_spi_exchange(PIC32SpiState *s, uint32_t value)
{
    unsigned width = pic32_spi_width(s);
    uint32_t in = 0;
    unsigned i;

    for (i = width; i; i -= 8) {
        in = (in << 8) | ssi_transfer(s->ssi, (value >> (i - 8)) & 0xFF);
    }

    if (s->rx_count == pic32_spi_fifo_depth(s)) {
        /*
         * Hardware discards the new data and latches the overrun, which stops
         * nothing: the guest keeps clocking and finds out when it reads STAT.
         */
        s->stat |= STAT_SPIROV;
        return;
    }
    s->rx[s->rx_count++] = in;
}

/*
 * One bit has finished. The line carries the next one, and a word running out
 * frees a place in the transmit buffer -- which is what the transfer-done
 * source reports, and so what a DMA channel feeding this controller waits for.
 */
static void pic32_spi_shift_tick(void *opaque)
{
    PIC32SpiState *s = opaque;

    if (s->bus_timed) {
        /*
         * The word's time on the wire is up: it reaches the device now, and
         * so does the answer. The transmit event follows, then the next word
         * if one is waiting.
         */
        s->shift_bits = 0;
        pic32_spi_exchange(s, s->shift_reg);
        pic32_spi_update_irq(s);
        qemu_irq_pulse(s->irq[PIC32_SPI_IRQ_TX]);
        pic32_spi_shift_start(s);
        return;
    }

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
    if (s->bus_timed) {
        /* Whole words at a time; shift_bits only marks the shifter busy. */
        s->shift_bits = width;
        timer_mod(s->shift, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            (int64_t)width * pic32_spi_bit_ns(s));
        return;
    }
    s->shift_bits = width - 1;
    pic32_spi_sdo(s, (s->shift_reg >> s->shift_bits) & 1);
    timer_mod(s->shift, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                        pic32_spi_bit_ns(s));
}

static void pic32_spi_transfer(PIC32SpiState *s, uint32_t value)
{
    if (s->serial_out || s->bus_timed) {
        /*
         * Nothing on the far end of a pin answers back, so there is no receive
         * side here: the word joins the queue for the shifter and the guest
         * finds out it has gone when the transfer-done source fires.
         */
        if (s->tx_count == pic32_spi_fifo_depth(s)) {
            /* A write to a full FIFO goes nowhere; the pointers don't move. */
            return;
        }
        s->tx[s->tx_count++] = value;
        if (!s->shift_bits && !timer_pending(s->shift)) {
            pic32_spi_shift_start(s);
        }
        return;
    }

    /* A bus-path controller: the exchange is over before the guest can look. */
    pic32_spi_exchange(s, value);
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
            /*
             * Turning the module off resets it: the shifter stops where it
             * is and both queues forget what they held, so a re-enable
             * starts clean instead of replaying the tail of an old burst.
             */
            timer_del(s->shift);
            s->shift_bits = 0;
            s->shift_reg = 0;
            s->tx_count = 0;
            s->rx_count = 0;
            s->stat = 0;
        } else {
            if (!(value & CON_MSTEN)) {
                qemu_log_mask(LOG_UNIMP,
                              "pic32-spi: client mode is not modelled\n");
            }
            if (s->serial_out && CON_STXISEL(value) >= 2 &&
                !s->stxisel_logged) {
                s->stxisel_logged = true;
                qemu_log_mask(LOG_UNIMP,
                              "pic32-spi: STXISEL=%u asserts on buffer space; "
                              "this model paces one transmit event per word\n",
                              (unsigned)CON_STXISEL(value));
            }
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
        s->brg = value & BRG_MASK;
        break;
    case R_CON2:
        s->con2 = value;
        pic32_spi_update_irq(s);
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
    /* Overflow and underrun report as error events out of reset. */
    s->con2 = CON2_SPIROVEN | CON2_SPITUREN;
    s->stat = 0;
    s->brg = 0;
    s->rx_count = 0;
    s->tx_count = 0;
    s->shift_bits = 0;
    if (s->shift) {
        timer_del(s->shift);
    }
    s->srxisel_logged = false;
    s->stxisel_logged = false;
    pic32_spi_update_irq(s);
}

static void pic32_spi_realize(DeviceState *dev, Error **errp)
{
    PIC32SpiState *s = PIC32_SPI(dev);

    if ((s->serial_out || s->bus_timed) && !clock_has_source(s->pbclk)) {
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
    /*
     * Set for a controller whose device on the bus has timing of its own to
     * keep: words reach it one at a time, each at the end of its time on the
     * wire, rather than the moment the guest writes SPIxBUF.
     */
    DEFINE_PROP_BOOL("bus-timed", PIC32SpiState, bus_timed, false),
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
