/*
 * PIC32 DMA controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32_DMAC_H
#define HW_PIC32_PIC32_DMAC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"

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

    /*
     * The previous byte of the stream, for the two-byte pattern match: the
     * matching pair can straddle two cells, so the tail byte of one step has
     * to survive into the next.
     */
    uint32_t pat_prev;

    /*
     * A start event that arrived while the channel was disabled. Silicon
     * keeps one and spends it as a first cell the moment the channel is
     * enabled -- measured on the PIC32MK: it fires on every arm except a
     * virgin port's first, which is why firmware that arms and then forces
     * gets two first cells. See "The phantom first cell" in pic32_dmac.c.
     */
    bool latched_start;

    /*
     * A block in flight against the virtual clock. The data has already been
     * moved -- see "Batched transfers" in pic32_dmac.c -- and what remains is
     * the time the bus cycles would have taken: the timer delivers the
     * completion at batch_end_ns, and until then the pointer registers are
     * computed from how far along the clock says the transfer is.
     */
    bool batch;
    uint32_t batch_cells;
    int64_t batch_start_ns;
    int64_t batch_end_ns;
    QEMUTimer *timer;

    /* For the timer callback and register reads to find their way home. */
    struct PIC32DmacState *parent;
    unsigned index;
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

    /* One complaint about BYTO on non-word transfers is enough. */
    bool byto_logged;

    PIC32DmacChannel ch[PIC32_DMAC_CHANNELS];

    qemu_irq irq[PIC32_DMAC_CHANNELS];

    /*
     * How long one cell takes when the named source triggers it, in
     * nanoseconds of guest time; zero means the source's timing is its own
     * (a timer-paced peripheral, or something genuinely asynchronous) and
     * the channel steps one cell per event. Configuration, wired by the SoC,
     * so not migrated.
     */
    uint32_t pacing_ns[256];
};

/*
 * An interrupt source has changed state. The controller watches the same
 * request lines the interrupt controller does -- a channel armed with
 * SIRQEN starts a cell transfer when the source it names goes active,
 * whether or not that source is enabled in IEC. Called by the EVIC, which is
 * where every source in the SoC already arrives.
 */
/*
 * An interrupt source changed, as seen by the channels' start and abort
 * detectors. Pulsed says whether this is a latching source's discrete event
 * (a port cycle finishing) or a level source's line (a FIFO staying ready);
 * only the former is a thing that can be missed, so only the former is
 * latched for a disabled channel.
 */
void pic32_dmac_irq_event(PIC32DmacState *s, unsigned source, bool level,
                          bool pulsed);

/*
 * Declares that the named source raises one event per cell moved, a fixed
 * cycle_ns apart -- true of a free-running bus like the parallel port in
 * master mode, and what lets the controller move the whole block at once and
 * charge the right amount of virtual time, instead of paying a scheduler
 * round-trip per cell. Called by the SoC at wiring time.
 */
void pic32_dmac_set_source_pacing(PIC32DmacState *s, unsigned source,
                                  uint32_t cycle_ns);

#endif /* HW_PIC32_PIC32_DMAC_H */
