/*
 * PIC32 register blocks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/pic32/pic32_regs.h"

typedef struct PIC32Regs {
    const PIC32RegsOps *ops;
    void *opaque;
    unsigned stride;
} PIC32Regs;

/* Which of the four addresses in an aliased register's slot was touched. */
enum {
    PIC32_PLAIN = 0,
    PIC32_CLR = 1,
    PIC32_SET = 2,
    PIC32_INV = 3,
};

static uint64_t pic32_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC32Regs *r = opaque;

    /*
     * The data sheet says reading an alias returns an undefined value. This
     * returns the register behind it, which is undefined in the same sense and
     * a good deal easier to read in a trace than a zero would be.
     */
    if (r->stride == PIC32_REGS_ALIASED) {
        addr &= ~(hwaddr)0xC;
    }
    return r->ops->read(r->opaque, addr);
}

static void pic32_regs_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    PIC32Regs *r = opaque;
    uint32_t val = value;

    if (r->stride == PIC32_REGS_ALIASED) {
        unsigned op = (addr >> 2) & 3;

        addr &= ~(hwaddr)0xC;
        switch (op) {
        case PIC32_CLR:
            val = r->ops->read(r->opaque, addr) & ~val;
            break;
        case PIC32_SET:
            val = r->ops->read(r->opaque, addr) | val;
            break;
        case PIC32_INV:
            val = r->ops->read(r->opaque, addr) ^ val;
            break;
        default:
            break;
        }
    }
    r->ops->write(r->opaque, addr, val);
}

static const MemoryRegionOps pic32_regs_mr_ops = {
    .read = pic32_regs_read,
    .write = pic32_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    /*
     * Every register is 32 bits wide even where firmware reaches for half of
     * one, so narrow accesses become read-modify-write of the whole thing.
     * That is what the bus does, and it is what makes WDT_Clear() -- a 16-bit
     * store of the key to WDTCON's upper half -- arrive as a write the
     * watchdog can recognise.
     */
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

void pic32_regs_init_io(MemoryRegion *mr, Object *owner,
                        const PIC32RegsOps *ops, void *opaque,
                        const char *name, uint64_t size, unsigned stride)
{
    PIC32Regs *r = g_new0(PIC32Regs, 1);

    assert(stride == PIC32_REGS_ALIASED || stride == PIC32_REGS_PLAIN);
    r->ops = ops;
    r->opaque = opaque;
    r->stride = stride;

    memory_region_init_io(mr, owner, &pic32_regs_mr_ops, r, name, size);
}
