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
    uint32_t spllcon_reset; /* what the DEVCFG fuses load into SPLLCON */

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

    /*
     * What RCON should add after the reset that is about to happen. The
     * cause has to be recorded before the reset and ORed in on the way back
     * up, on top of whatever earlier causes software has not yet cleared.
     */
    uint32_t rcon_pending;

    /* Told whenever CFGCON or a PMD register changes, however it changes. */
    void (*cfg_notify)(void *opaque);
    void *cfg_notify_opaque;
};

/* CFGCON bits someone other than this module acts on. */
#define PIC32_CFGCON_IOLOCK (1u << 13)

/*
 * Registers the hook called when CFGCON or a PMD register takes a new value,
 * whether from the guest, from reset or from an incoming migration, so what
 * the SoC derives from them never goes stale.
 */
void pic32_cru_set_cfg_notify(PIC32CruState *s, void (*fn)(void *opaque),
                              void *opaque);

/* True while the guest holds the system unlock. */
bool pic32_cru_unlocked(PIC32CruState *s);

/* RCON bits, for whoever is about to cause a reset. */
#define PIC32_RCON_POR  (1u << 0)
#define PIC32_RCON_BOR  (1u << 1)
#define PIC32_RCON_WDTO (1u << 4)
#define PIC32_RCON_SWR  (1u << 6)

/* Says what the coming reset should be blamed on. */
void pic32_cru_set_reset_cause(PIC32CruState *s, uint32_t rcon_bits);

#endif /* HW_PIC32_PIC32_CRU_H */
