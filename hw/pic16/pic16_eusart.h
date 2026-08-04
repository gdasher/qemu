/*
 * PIC16 EUSART
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_EUSART_H
#define HW_PIC16_EUSART_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_PIC16_EUSART "pic16-eusart"
OBJECT_DECLARE_SIMPLE_TYPE(PIC16EusartState, PIC16_EUSART)

struct PIC16EusartState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharFrontend chr;

    uint8_t rcreg;
    uint8_t brgl;
    uint8_t brgh;
    uint8_t rcsta;
    uint8_t txsta;
    uint8_t baudcon;

    bool rx_full;

    qemu_irq tx_irq;    /* TXxIF, held while the transmitter is ready */
    qemu_irq rx_irq;    /* RCxIF, held while a byte is waiting */
};

#endif /* HW_PIC16_EUSART_H */
