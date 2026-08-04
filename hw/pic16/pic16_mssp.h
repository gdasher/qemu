/*
 * PIC16 MSSP in SPI host mode
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC16_MSSP_H
#define HW_PIC16_MSSP_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_PIC16_MSSP "pic16-mssp"
OBJECT_DECLARE_SIMPLE_TYPE(PIC16MsspState, PIC16_MSSP)

struct PIC16MsspState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SSIBus *ssi;

    uint8_t buf;
    uint8_t add;
    uint8_t msk;
    uint8_t stat;
    uint8_t con1;
    uint8_t con2;
    uint8_t con3;
};

#endif /* HW_PIC16_MSSP_H */
