/*
 * Analog Devices ADF4002 Phase-Locked Loop (PLL) Synthesizer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHIPS_ADF4002_H
#define HW_CHIPS_ADF4002_H

#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_ADF4002 "adf4002"
OBJECT_DECLARE_SIMPLE_TYPE(ADF4002State, ADF4002)

#define ADF4002_MUX_OUT_GPIO "mux-out"
#define ADF4002_LE_GPIO      "le"
#define ADF4002_CE_GPIO      "ce"

struct ADF4002State {
    SSIPeripheral parent_obj;

    uint32_t shift_reg;

    uint32_t r_latch;
    uint32_t n_latch;
    uint32_t func_latch;
    uint32_t init_latch;

    uint16_t r_counter;
    uint16_t n_counter;

    bool ce;
    bool le;
    bool locked;
    bool mux_out_level;   /* level currently driven on MUXOUT */

    qemu_irq mux_out;

    void (*latch_sink)(void *opaque, uint16_t r, uint16_t n, uint32_t func, bool locked);
    void *latch_sink_opaque;
};

void adf4002_set_latch_sink(ADF4002State *s,
                            void (*sink)(void *opaque, uint16_t r, uint16_t n, uint32_t func, bool locked),
                            void *opaque);

#endif /* HW_CHIPS_ADF4002_H */
