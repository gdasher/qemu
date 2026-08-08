/*
 * PIC32 clock, reset and system configuration
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32_CRU_H
#define HW_PIC32_PIC32_CRU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PIC32_CRU "pic32-cru"
OBJECT_DECLARE_SIMPLE_TYPE(PIC32CruState, PIC32_CRU)

/* The two blocks this covers: CFG-PMD at 0x0000 and CRU at 0x1200. */
#define PIC32_CRU_CFG_SIZE 0x200
#define PIC32_CRU_OSC_SIZE 0x200

#define PIC32_CRU_PMDS 7
#define PIC32_CRU_REFOS 4
#define PIC32_CRU_PBDIVS 7

struct PIC32CruState {
    SysBusDevice parent_obj;

    MemoryRegion cfg;
    MemoryRegion osc;

    uint32_t devid;     /* what DEVID reads back, from the part */

    uint32_t cfgcon;
    uint32_t cfgcon2;
    uint32_t cfgpg;
    uint32_t cfgapp2;
    uint32_t syskey;
    uint32_t pmd[PIC32_CRU_PMDS];

    uint32_t osccon;
    uint32_t osctun;
    uint32_t spllcon;
    uint32_t upllcon;
    uint32_t rcon;
    uint32_t rnmicon;
    uint32_t pwrcon;
    uint32_t refocon[PIC32_CRU_REFOS];
    uint32_t refotrim[PIC32_CRU_REFOS];
    uint32_t pbdiv[PIC32_CRU_PBDIVS];
    uint32_t slewcon;

    /* How far through the 0xAA996655, 0x556699AA sequence SYSKEY is. */
    uint32_t unlock_step;
};

/* True while the guest holds the system unlock. */
bool pic32_cru_unlocked(PIC32CruState *s);

#endif /* HW_PIC32_PIC32_CRU_H */
