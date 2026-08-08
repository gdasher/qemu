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

/* The plain register an address belongs to, and which alias of it it is. */
static hwaddr pic32_regs_base(PIC32Regs *r, hwaddr addr)
{
    return addr & ~(hwaddr)(r->stride == PIC32_REGS_ALIASED ? 0xF : 0x3);
}

static unsigned pic32_regs_alias(PIC32Regs *r, hwaddr addr)
{
    return r->stride == PIC32_REGS_ALIASED ? (addr >> 2) & 3 : PIC32_PLAIN;
}

static uint64_t pic32_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC32Regs *r = opaque;
    unsigned shift = (addr & 3) * 8;

    /*
     * The data sheet says reading an alias returns an undefined value. This
     * returns the register behind it, which is undefined in the same sense and
     * a good deal easier to read in a trace than a zero would be.
     */
    return (r->ops->read(r->opaque, pic32_regs_base(r, addr)) >> shift) &
           MAKE_64BIT_MASK(0, size * 8);
}

static void pic32_regs_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    PIC32Regs *r = opaque;
    hwaddr base = pic32_regs_base(r, addr);
    unsigned alias = pic32_regs_alias(r, addr);
    unsigned shift = (addr & 3) * 8;
    uint32_t mask = MAKE_64BIT_MASK(shift, size * 8);
    uint32_t val = (uint32_t)(value << shift) & mask;

    /*
     * Firmware writes parts of registers: the startup code copies the
     * interrupt vector offsets in a byte at a time, and the watchdog is fed by
     * storing a key to the top half of WDTCON. So a narrow access has to be
     * merged rather than widened, which is what the memory core would do on
     * its own.
     *
     * What the untouched bytes should hold depends on which register was
     * addressed. For the plain one they keep what is already there. For a
     * clear, set or invert alias they are zero, because a zero bit in those is
     * "leave this one alone" -- filling them in from the register would turn a
     * one-byte set into a request to set every bit already set, or a one-byte
     * invert into one that inverted three bytes it was never asked about.
     */
    if (size < 4 && alias == PIC32_PLAIN) {
        val |= r->ops->read(r->opaque, base) & ~mask;
    }

    switch (alias) {
    case PIC32_CLR:
        val = r->ops->read(r->opaque, base) & ~val;
        break;
    case PIC32_SET:
        val = r->ops->read(r->opaque, base) | val;
        break;
    case PIC32_INV:
        val = r->ops->read(r->opaque, base) ^ val;
        break;
    default:
        break;
    }
    r->ops->write(r->opaque, base, val);
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
     * Narrow accesses arrive as they were made rather than widened, because
     * only this code knows how to merge one into the register behind it.
     */
    .impl = {
        .min_access_size = 1,
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
