/*
 * Asynchronous SRAM on a parallel address and data bus
 *
 * The kind of part a microcontroller hangs off a parallel port when it needs
 * more memory than it has: an address bus, a data bus, and no protocol at all.
 * It is not memory-mapped in the guest's address space -- the controller
 * reaches it a word at a time -- so it is a plain array here rather than a
 * MemoryRegion.
 *
 * A board that has more address lines than the part has locations can use one
 * of the spare ones as a chip select, which is what the XMASNg board does with
 * A23. Declaring it here means an address with the wrong select is a complaint
 * rather than an aliased access into the middle of the array.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/chips/parallel_sram.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

static bool parallel_sram_selected(ParallelSramState *s, uint32_t addr)
{
    if (!s->select) {
        return true;
    }
    if (addr & s->select) {
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "parallel-sram: address 0x%06x does not select this chip\n",
                  addr);
    return false;
}

static uint32_t parallel_sram_index(ParallelSramState *s, uint32_t addr)
{
    return (addr & ~s->select) % s->words;
}

uint16_t parallel_sram_read(void *opaque, uint32_t addr)
{
    ParallelSramState *s = opaque;

    if (!parallel_sram_selected(s, addr)) {
        return 0xFFFF;
    }
    return s->data[parallel_sram_index(s, addr)];
}

void parallel_sram_write(void *opaque, uint32_t addr, uint16_t data)
{
    ParallelSramState *s = opaque;

    if (!parallel_sram_selected(s, addr)) {
        return;
    }
    s->data[parallel_sram_index(s, addr)] =
        s->width == 8 ? data & 0xFF : data;
}

static void parallel_sram_realize(DeviceState *dev, Error **errp)
{
    ParallelSramState *s = PARALLEL_SRAM(dev);

    if (s->width != 8 && s->width != 16) {
        error_setg(errp, "parallel-sram: width must be 8 or 16");
        return;
    }
    if (!s->words) {
        error_setg(errp, "parallel-sram: needs a size");
        return;
    }
    /*
     * Not zeroed on reset: an SRAM keeps whatever it held as long as it is
     * powered, and the board's power outlives the microcontroller's reset.
     */
    s->data = g_new0(uint16_t, s->words);
}

static void parallel_sram_unrealize(DeviceState *dev)
{
    ParallelSramState *s = PARALLEL_SRAM(dev);

    g_free(s->data);
}

static const Property parallel_sram_properties[] = {
    DEFINE_PROP_UINT64("words", ParallelSramState, words, 0),
    DEFINE_PROP_UINT32("width", ParallelSramState, width, 16),
    DEFINE_PROP_UINT32("select", ParallelSramState, select, 0),
};

static void parallel_sram_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = parallel_sram_realize;
    dc->unrealize = parallel_sram_unrealize;
    dc->user_creatable = false;
    device_class_set_props(dc, parallel_sram_properties);
}

static const TypeInfo parallel_sram_types[] = {
    {
        .name = TYPE_PARALLEL_SRAM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(ParallelSramState),
        .class_init = parallel_sram_class_init,
    },
};

DEFINE_TYPES(parallel_sram_types)
