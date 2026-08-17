/*
 * PIC16 MSSP in SPI host and slave modes
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_MSSP_H
#define HW_PIC16_MSSP_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_PIC16_MSSP "pic16-mssp"
OBJECT_DECLARE_SIMPLE_TYPE(PIC16MsspState, PIC16_MSSP)

struct PIC16MsspState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SSIBus *ssi;
    CharFrontend chr;

    /*
     * Something outside is the master: a chardev client, or the
     * co-simulation link, which clocks bytes in through
     * pic16_mssp_slave_transfer() rather than through a chardev of its own.
     * Either way SSPxBUF preloads a reply instead of starting a transfer.
     */
    bool external;

    uint8_t buf;
    uint8_t tx_buf;
    uint8_t add;
    uint8_t msk;
    uint8_t stat;
    uint8_t con1;
    uint8_t con2;
    uint8_t con3;

    qemu_irq irq;
};

uint8_t pic16_mssp_slave_transfer(PIC16MsspState *s, uint8_t in_byte);

#endif /* HW_PIC16_MSSP_H */
