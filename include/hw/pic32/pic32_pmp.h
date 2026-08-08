/*
 * PIC32 parallel master port
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PIC32_PIC32_PMP_H
#define HW_PIC32_PIC32_PMP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PIC32_PMP "pic32-pmp"
OBJECT_DECLARE_SIMPLE_TYPE(PIC32PmpState, PIC32_PMP)

#define PIC32_PMP_SIZE 0xA0

#define PIC32_PMP_IRQ_GPIO "irq"

/*
 * What is on the other side of the port. The controller drives an address and
 * either latches a word or presents one; what answers is a chip on the board,
 * so the board says what that is.
 */
typedef struct PIC32PmpTarget {
    uint16_t (*read)(void *opaque, uint32_t addr);
    void (*write)(void *opaque, uint32_t addr, uint16_t data);
} PIC32PmpTarget;

struct PIC32PmpState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    const PIC32PmpTarget *target;
    void *target_opaque;

    uint32_t con;
    uint32_t mode;
    uint32_t addr;
    uint32_t aen;
    uint32_t stat;
    uint32_t waddr;
    uint32_t raddr;
    uint32_t dout;

    /*
     * What the last read cycle fetched. Reading the data register hands this
     * back and starts the next cycle, which is one word behind the address --
     * see the note in pic32_pmp.c.
     */
    uint32_t din;

    qemu_irq irq;
};

/* Says what the port is wired to. Call before realize. */
void pic32_pmp_attach(PIC32PmpState *s, const PIC32PmpTarget *target,
                      void *opaque);

#endif /* HW_PIC32_PIC32_PMP_H */
