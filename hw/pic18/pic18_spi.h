/*
 * PIC18F-Q43 SPI module (SPI1/SPI2) in host and client modes
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC18_SPI_H
#define HW_PIC18_SPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_PIC18_SPI "pic18-spi"
OBJECT_DECLARE_SIMPLE_TYPE(PIC18SpiState, PIC18_SPI)

/*
 * Host-mode SS(out), as the electrical level the pin would show (SSP decides
 * which level is "asserted"). The SoC's PPS routing puts it on a port pin.
 */
#define PIC18_SPI_SS_OUT_GPIO "ss"

/* The client-select input pin's level, for SOSIF/EOSIF. */
#define PIC18_SPI_SS_IN_GPIO "ss-in"

#define PIC18_SPI_FIFO_DEPTH 2

/* Sysbus IRQ outputs, all level lines. */
enum {
    PIC18_SPI_IRQ_RX,   /* SPIxRXIF: receive FIFO not empty */
    PIC18_SPI_IRQ_TX,   /* SPIxTXIF: transmit FIFO not full */
    PIC18_SPI_IRQ_GEN,  /* SPIxIF: any SPIxINTF & SPIxINTE */
    PIC18_SPI_NUM_IRQ,
};

struct PIC18SpiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SSIBus *ssi;
    CharFrontend chr;

    /*
     * Something outside is the master and clocks bytes in through
     * pic18_spi_client_transfer(): a chardev client or the co-simulation
     * link. Kept for the link's benefit; MST decides the mode.
     */
    bool external;

    uint8_t rx_fifo[PIC18_SPI_FIFO_DEPTH];
    uint8_t rx_n;
    uint8_t tx_fifo[PIC18_SPI_FIFO_DEPTH];
    uint8_t tx_n;
    uint8_t last_rx;    /* what a client sends when its FIFO runs dry */

    uint16_t tcnt;      /* 11-bit transfer counter */
    uint8_t con0;
    uint8_t con1;
    uint8_t con2;
    uint8_t status;     /* TXWE and RXRE; the rest is derived */
    uint8_t twidth;
    uint8_t baud;
    uint8_t intf;
    uint8_t inte;
    uint8_t clk;

    bool ss_in;         /* the select input pin's level */
    bool ss_out;        /* the level driven on SS(out) */

    qemu_irq irq[PIC18_SPI_NUM_IRQ];
    qemu_irq ss_out_irq;
};

/**
 * pic18_spi_client_transfer:
 *
 * An external master clocks one byte in and takes the client's reply: the
 * oldest byte in the transmit FIFO, or the most recently received byte with
 * TXUIF set when the FIFO is empty. The byte goes into the receive FIFO if
 * there is room, and is dropped with RXOIF set otherwise.
 */
uint8_t pic18_spi_client_transfer(PIC18SpiState *s, uint8_t in);

#endif /* HW_PIC18_SPI_H */
