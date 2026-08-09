/*
 * PIC32 DMA controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32_DMAC_H
#define HW_PIC32_PIC32_DMAC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PIC32_DMAC "pic32-dmac"
OBJECT_DECLARE_SIMPLE_TYPE(PIC32DmacState, PIC32_DMAC)

#define PIC32_DMAC_CHANNELS 8

/*
 * Global registers at 0x00, then one 0xC0 block per channel from 0x60. Every
 * register has the usual CLR/SET/INV aliases, so the block is laid out on the
 * 16-byte stride the register fabric expects.
 */
#define PIC32_DMAC_SIZE (0x60 + PIC32_DMAC_CHANNELS * 0xC0)

/* One line per channel, to the interrupt controller. */
#define PIC32_DMAC_IRQ_GPIO "irq"

typedef struct PIC32DmacChannel {
    uint32_t con;
    uint32_t econ;
    uint32_t intr;
    uint32_t ssa;
    uint32_t dsa;
    uint32_t ssiz;
    uint32_t dsiz;
    uint32_t sptr;
    uint32_t dptr;
    uint32_t csiz;
    uint32_t cptr;
    uint32_t dat;
} PIC32DmacChannel;

struct PIC32DmacState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint32_t dmacon;
    uint32_t dmastat;
    uint32_t dmaaddr;
    uint32_t dcrccon;
    uint32_t dcrcdata;
    uint32_t dcrcxor;

    PIC32DmacChannel ch[PIC32_DMAC_CHANNELS];

    qemu_irq irq[PIC32_DMAC_CHANNELS];
};

/*
 * An interrupt source has changed state. The controller watches the same
 * request lines the interrupt controller does -- a channel armed with
 * SIRQEN starts a cell transfer when the source it names goes active,
 * whether or not that source is enabled in IEC. Called by the EVIC, which is
 * where every source in the SoC already arrives.
 */
void pic32_dmac_irq_event(PIC32DmacState *s, unsigned source, bool level);

#endif /* HW_PIC32_PIC32_DMAC_H */
