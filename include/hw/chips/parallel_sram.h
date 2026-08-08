/*
 * Asynchronous SRAM on a parallel address and data bus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHIPS_PARALLEL_SRAM_H
#define HW_CHIPS_PARALLEL_SRAM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PARALLEL_SRAM "parallel-sram"
OBJECT_DECLARE_SIMPLE_TYPE(ParallelSramState, PARALLEL_SRAM)

struct ParallelSramState {
    SysBusDevice parent_obj;

    uint64_t words;     /* how many addressable locations it has */
    uint32_t width;     /* 8 or 16 bits to a location */
    uint32_t select;    /* an address line used as chip select, or 0 for none */

    uint16_t *data;
};

/*
 * Reads and writes one location. Both are given the address the controller
 * drove, chip select line and all.
 */
uint16_t parallel_sram_read(void *opaque, uint32_t addr);
void parallel_sram_write(void *opaque, uint32_t addr, uint16_t data);

#endif /* HW_CHIPS_PARALLEL_SRAM_H */
