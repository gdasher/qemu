/*
 * PIC18F-Q43 SPI module (SPI1/SPI2) in host and client modes
 *
 * Both directions have a two-byte FIFO. In host mode (MST = 1) a byte written
 * to SPIxTXB is shifted out through the SSI bus at once -- as long as TXR is
 * set and, with BMODE = 0, the transfer counter is nonzero -- and the byte
 * clocked back is kept when RXR is set. Loading the counter asserts SS(out);
 * it deasserts when the counter reaches zero (TCZIF) or, with SSET, never.
 * Transfers complete immediately, so under -icount the whole frame is
 * deterministic and the firmware's TXIF/TXBE/BUSY polling falls straight
 * through.
 *
 * In client mode (MST = 0) bytes arrive from outside -- a chardev streaming
 * master bytes, or the co-simulation link -- through
 * pic18_spi_client_transfer(), which answers with the head of the transmit
 * FIFO or, when it is empty, the last byte received with TXUIF set; a byte
 * arriving into a full receive FIFO is lost with RXOIF set. That is what the
 * silicon does and what the reference firmware's status bits report.
 *
 * The select input is tracked only for SOSIF/EOSIF; it does not gate client
 * transfers, so a plain byte-stream master with no select line works exactly
 * as it does against the PIC16 MSSP model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "pic18_spi.h"

enum {
    REG_RXB,
    REG_TXB,
    REG_TCNTL,
    REG_TCNTH,
    REG_CON0,
    REG_CON1,
    REG_CON2,
    REG_STATUS,
    REG_TWIDTH,
    REG_BAUD,
    REG_INTF,
    REG_INTE,
    REG_CLK,
    PIC18_SPI_NREGS,
};

#define CON0_EN     (1u << 7)
#define CON0_LSBF   (1u << 2)
#define CON0_MST    (1u << 1)
#define CON0_BMODE  (1u << 0)

#define CON1_SMP    (1u << 7)
#define CON1_CKE    (1u << 6)
#define CON1_CKP    (1u << 5)
#define CON1_FST    (1u << 4)
#define CON1_SSP    (1u << 2)
#define CON1_SDIP   (1u << 1)
#define CON1_SDOP   (1u << 0)

#define CON2_BUSY   (1u << 7)
#define CON2_SSFLT  (1u << 6)
#define CON2_SSET   (1u << 2)
#define CON2_TXR    (1u << 1)
#define CON2_RXR    (1u << 0)
#define CON2_WMASK  (CON2_SSFLT | CON2_SSET | CON2_TXR | CON2_RXR)

#define STATUS_TXWE (1u << 7)
#define STATUS_TXBE (1u << 5)
#define STATUS_RXRE (1u << 3)
#define STATUS_CLRBF (1u << 2)
#define STATUS_RXBF (1u << 0)

#define INTF_SRMTIF (1u << 7)
#define INTF_TCZIF  (1u << 6)
#define INTF_SOSIF  (1u << 5)
#define INTF_EOSIF  (1u << 4)
#define INTF_RXOIF  (1u << 2)
#define INTF_TXUIF  (1u << 1)
#define INTF_MASK   0xF6

#define TCNT_MASK   0x7FF

static bool pic18_spi_enabled(PIC18SpiState *s)
{
    return (s->con0 & CON0_EN) != 0;
}

static bool pic18_spi_is_host(PIC18SpiState *s)
{
    return (s->con0 & CON0_MST) != 0;
}

static void pic18_spi_update_irq(PIC18SpiState *s)
{
    qemu_set_irq(s->irq[PIC18_SPI_IRQ_RX], s->rx_n > 0);
    qemu_set_irq(s->irq[PIC18_SPI_IRQ_TX], s->tx_n < PIC18_SPI_FIFO_DEPTH);
    qemu_set_irq(s->irq[PIC18_SPI_IRQ_GEN], (s->intf & s->inte) != 0);
}

/*
 * SS(out) is asserted while SSET is set or the transfer counter is nonzero;
 * SSP picks the asserted level (0: active low). The pin only shows this when
 * the SoC's PPS routing sends it there.
 */
static void pic18_spi_update_ss_out(PIC18SpiState *s)
{
    bool asserted = pic18_spi_is_host(s) && pic18_spi_enabled(s) &&
                    ((s->con2 & CON2_SSET) || s->tcnt != 0);
    bool level = asserted ? !!(s->con1 & CON1_SSP) : !(s->con1 & CON1_SSP);

    if (level != s->ss_out) {
        s->ss_out = level;
        qemu_set_irq(s->ss_out_irq, level);
    }
}

static void pic18_spi_rx_push(PIC18SpiState *s, uint8_t byte)
{
    if (s->rx_n < PIC18_SPI_FIFO_DEPTH) {
        s->rx_fifo[s->rx_n++] = byte;
    } else {
        s->intf |= INTF_RXOIF;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pic18-spi: rx overflow at %" PRId64 " (byte %02x lost)\n",
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), byte);
    }
}

static uint8_t pic18_spi_tx_pop(PIC18SpiState *s)
{
    uint8_t byte = s->tx_fifo[0];

    s->tx_fifo[0] = s->tx_fifo[1];
    s->tx_n--;
    return byte;
}

/* The counter decrements on every transfer, in both modes, and rolls over. */
static void pic18_spi_count(PIC18SpiState *s)
{
    if (s->tcnt == 1) {
        s->intf |= INTF_TCZIF;
    }
    s->tcnt = (s->tcnt - 1) & TCNT_MASK;
}

/*
 * Host mode: run every transfer the FIFOs and the counter allow. With TXR
 * set, each is a byte from the transmit FIFO; with only RXR set, the module
 * clocks zeros out to fetch bytes in.
 */
static void pic18_spi_host_run(PIC18SpiState *s)
{
    if (!pic18_spi_enabled(s) || !pic18_spi_is_host(s)) {
        return;
    }
    if (!(s->con2 & (CON2_TXR | CON2_RXR))) {
        return;     /* transfer off: bytes queue in the FIFO */
    }

    for (;;) {
        bool bmode = (s->con0 & CON0_BMODE) != 0;
        uint8_t out, in;

        if (!bmode && s->tcnt == 0) {
            break;
        }
        if ((s->con2 & CON2_RXR) && s->rx_n == PIC18_SPI_FIFO_DEPTH) {
            break;  /* a host never overflows its receive FIFO */
        }
        if (s->con2 & CON2_TXR) {
            if (s->tx_n == 0) {
                break;
            }
            out = pic18_spi_tx_pop(s);
        } else {
            /* Receive only, driven by the counter alone. */
            if (bmode) {
                break;
            }
            out = 0;
        }

        pic18_spi_update_ss_out(s);
        in = ssi_transfer(s->ssi, out);
        if (s->con2 & CON2_RXR) {
            pic18_spi_rx_push(s, in);
        }
        s->last_rx = in;
        s->intf |= INTF_SRMTIF;
        pic18_spi_count(s);
    }
    pic18_spi_update_ss_out(s);
    pic18_spi_update_irq(s);
}

uint8_t pic18_spi_client_transfer(PIC18SpiState *s, uint8_t in)
{
    uint8_t out;

    if (!pic18_spi_enabled(s)) {
        return 0xFF;    /* SDO tri-stated; the line idles high */
    }

    if (s->con2 & CON2_TXR) {
        if (s->tx_n > 0) {
            out = pic18_spi_tx_pop(s);
        } else {
            out = s->last_rx;
            s->intf |= INTF_TXUIF;
        }
    } else {
        out = s->tx_n > 0 ? s->tx_fifo[0] : s->last_rx;
    }

    if (s->con2 & CON2_RXR) {
        pic18_spi_rx_push(s, in);
    }
    s->last_rx = in;
    s->intf |= INTF_SRMTIF;
    pic18_spi_count(s);
    pic18_spi_update_irq(s);

    return out;
}

static int pic18_spi_chr_can_receive(void *opaque)
{
    PIC18SpiState *s = opaque;

    if (!pic18_spi_enabled(s) || pic18_spi_is_host(s)) {
        return 0;
    }
    return PIC18_SPI_FIFO_DEPTH - s->rx_n;
}

static void pic18_spi_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    PIC18SpiState *s = opaque;

    for (int i = 0; i < size; i++) {
        uint8_t resp = pic18_spi_client_transfer(s, buf[i]);

        if (qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_write_all(&s->chr, &resp, 1);
        }
    }
}

static void pic18_spi_accept_input(PIC18SpiState *s)
{
    if (qemu_chr_fe_backend_connected(&s->chr) &&
        pic18_spi_chr_can_receive(s) > 0) {
        qemu_chr_fe_accept_input(&s->chr);
    }
}

static void pic18_spi_clear_fifos(PIC18SpiState *s)
{
    s->rx_n = 0;
    s->tx_n = 0;
    memset(s->rx_fifo, 0, sizeof(s->rx_fifo));
    memset(s->tx_fifo, 0, sizeof(s->tx_fifo));
}

static uint64_t pic18_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC18SpiState *s = opaque;
    uint8_t value;

    switch (addr) {
    case REG_RXB:
        if (s->rx_n == 0) {
            s->status |= STATUS_RXRE;
            return 0;
        }
        value = s->rx_fifo[0];
        s->rx_fifo[0] = s->rx_fifo[1];
        s->rx_n--;
        pic18_spi_update_irq(s);
        pic18_spi_accept_input(s);
        return value;
    case REG_TXB:
        /* Reading the transmit register returns the oldest queued byte. */
        return s->tx_n ? s->tx_fifo[0] : 0;
    case REG_TCNTL:
        return s->tcnt & 0xFF;
    case REG_TCNTH:
        return (s->tcnt >> 8) & 0x07;
    case REG_CON0:
        return s->con0;
    case REG_CON1:
        return s->con1;
    case REG_CON2:
        /* Transfers complete at once, so the module is never busy. */
        return s->con2 & CON2_WMASK;
    case REG_STATUS:
        value = s->status & (STATUS_TXWE | STATUS_RXRE);
        if (s->tx_n == 0) {
            value |= STATUS_TXBE;
        }
        if (s->rx_n == PIC18_SPI_FIFO_DEPTH) {
            value |= STATUS_RXBF;
        }
        return value;
    case REG_TWIDTH:
        return s->twidth;
    case REG_BAUD:
        return s->baud;
    case REG_INTF:
        return s->intf;
    case REG_INTE:
        return s->inte;
    case REG_CLK:
        return s->clk;
    default:
        return 0;
    }
}

static void pic18_spi_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    PIC18SpiState *s = opaque;

    switch (addr) {
    case REG_RXB:
        break;      /* read only */
    case REG_TXB:
        if (s->tx_n < PIC18_SPI_FIFO_DEPTH) {
            s->tx_fifo[s->tx_n++] = value;
        } else {
            s->status |= STATUS_TXWE;
        }
        pic18_spi_host_run(s);
        pic18_spi_update_irq(s);
        break;
    case REG_TCNTL:
        s->tcnt = (s->tcnt & 0x700) | (value & 0xFF);
        pic18_spi_host_run(s);
        pic18_spi_update_ss_out(s);
        break;
    case REG_TCNTH:
        s->tcnt = (s->tcnt & 0xFF) | ((value & 0x07) << 8);
        pic18_spi_update_ss_out(s);
        break;
    case REG_CON0:
        s->con0 = value & (CON0_EN | CON0_LSBF | CON0_MST | CON0_BMODE);
        pic18_spi_update_ss_out(s);
        pic18_spi_host_run(s);
        pic18_spi_accept_input(s);
        break;
    case REG_CON1:
        s->con1 = value;
        pic18_spi_update_ss_out(s);
        break;
    case REG_CON2:
        s->con2 = value & CON2_WMASK;
        pic18_spi_host_run(s);
        break;
    case REG_STATUS:
        if (value & STATUS_CLRBF) {
            pic18_spi_clear_fifos(s);
        }
        /* TXWE and RXRE are cleared by writing them 0. */
        s->status &= value & (STATUS_TXWE | STATUS_RXRE);
        pic18_spi_update_irq(s);
        pic18_spi_accept_input(s);
        break;
    case REG_TWIDTH:
        s->twidth = value & 0x07;
        break;
    case REG_BAUD:
        s->baud = value;
        break;
    case REG_INTF:
        s->intf = value & INTF_MASK;
        pic18_spi_update_irq(s);
        break;
    case REG_INTE:
        s->inte = value & INTF_MASK;
        pic18_spi_update_irq(s);
        break;
    case REG_CLK:
        s->clk = value & 0x0F;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pic18_spi_ops = {
    .read = pic18_spi_read,
    .write = pic18_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/*
 * The select input: SOSIF on its leading edge, EOSIF on its trailing edge,
 * with SSP saying which way round "active" is.
 */
static void pic18_spi_set_ss_in(void *opaque, int line, int level)
{
    PIC18SpiState *s = opaque;
    bool was_active = s->ss_in == !!(s->con1 & CON1_SSP);
    bool active;

    s->ss_in = level != 0;
    active = s->ss_in == !!(s->con1 & CON1_SSP);
    if (active && !was_active) {
        s->intf |= INTF_SOSIF;
    } else if (!active && was_active) {
        s->intf |= INTF_EOSIF;
    } else {
        return;
    }
    pic18_spi_update_irq(s);
}

static void pic18_spi_reset_hold(Object *obj, ResetType type)
{
    PIC18SpiState *s = PIC18_SPI(obj);

    pic18_spi_clear_fifos(s);
    s->last_rx = 0;
    s->tcnt = 0;
    s->con0 = 0;
    s->con1 = 0;
    s->con2 = 0;
    s->status = 0;
    s->twidth = 0;
    s->baud = 0;
    s->intf = 0;
    s->inte = 0;
    s->clk = 0;
    s->ss_out = false;
    pic18_spi_update_ss_out(s);
    pic18_spi_update_irq(s);
}

static void pic18_spi_realize(DeviceState *dev, Error **errp)
{
    PIC18SpiState *s = PIC18_SPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->ssi = ssi_create_bus(dev, "ssi");

    memory_region_init_io(&s->iomem, OBJECT(dev), &pic18_spi_ops, s,
                          "pic18.spi", PIC18_SPI_NREGS);
    sysbus_init_mmio(sbd, &s->iomem);
    for (unsigned i = 0; i < PIC18_SPI_NUM_IRQ; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
    qdev_init_gpio_out_named(dev, &s->ss_out_irq, PIC18_SPI_SS_OUT_GPIO, 1);
    qdev_init_gpio_in_named(dev, pic18_spi_set_ss_in, PIC18_SPI_SS_IN_GPIO, 1);

    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_set_handlers(&s->chr, pic18_spi_chr_can_receive,
                                 pic18_spi_chr_receive, NULL, NULL, s,
                                 NULL, true);
    }
}

static const Property pic18_spi_properties[] = {
    DEFINE_PROP_CHR("chardev", PIC18SpiState, chr),
};

static const VMStateDescription pic18_spi_vmstate = {
    .name = "pic18-spi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(rx_fifo, PIC18SpiState, PIC18_SPI_FIFO_DEPTH),
        VMSTATE_UINT8(rx_n, PIC18SpiState),
        VMSTATE_UINT8_ARRAY(tx_fifo, PIC18SpiState, PIC18_SPI_FIFO_DEPTH),
        VMSTATE_UINT8(tx_n, PIC18SpiState),
        VMSTATE_UINT8(last_rx, PIC18SpiState),
        VMSTATE_UINT16(tcnt, PIC18SpiState),
        VMSTATE_UINT8(con0, PIC18SpiState),
        VMSTATE_UINT8(con1, PIC18SpiState),
        VMSTATE_UINT8(con2, PIC18SpiState),
        VMSTATE_UINT8(status, PIC18SpiState),
        VMSTATE_UINT8(twidth, PIC18SpiState),
        VMSTATE_UINT8(baud, PIC18SpiState),
        VMSTATE_UINT8(intf, PIC18SpiState),
        VMSTATE_UINT8(inte, PIC18SpiState),
        VMSTATE_UINT8(clk, PIC18SpiState),
        VMSTATE_BOOL(ss_in, PIC18SpiState),
        VMSTATE_BOOL(ss_out, PIC18SpiState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic18_spi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic18_spi_realize;
    dc->vmsd = &pic18_spi_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, pic18_spi_properties);
    rc->phases.hold = pic18_spi_reset_hold;
}

static const TypeInfo pic18_spi_types[] = {
    {
        .name = TYPE_PIC18_SPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC18SpiState),
        .class_init = pic18_spi_class_init,
    },
};

DEFINE_TYPES(pic18_spi_types)
