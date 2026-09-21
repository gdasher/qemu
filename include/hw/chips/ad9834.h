/*
 * Analog Devices AD9834 direct digital synthesizer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHIPS_AD9834_H
#define HW_CHIPS_AD9834_H

#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_AD9834 "ad9834"
OBJECT_DECLARE_SIMPLE_TYPE(AD9834State, AD9834)

/*
 * FSYNC is the SSI chip select (SSI_GPIO_CS, active low): a board wires the
 * port pin carrying FSYNC to it, and the chip takes 16-bit words while it is
 * low. There is no separate "fsync" GPIO.
 */

/* Control register bits (D15:D14 = 00). */
#define AD9834_CTRL_B28       0x2000
#define AD9834_CTRL_HLB       0x1000
#define AD9834_CTRL_FSEL      0x0800
#define AD9834_CTRL_PSEL      0x0400
#define AD9834_CTRL_PINSW     0x0200
#define AD9834_CTRL_RESET     0x0100
#define AD9834_CTRL_SLEEP1    0x0080
#define AD9834_CTRL_SLEEP12   0x0040
#define AD9834_CTRL_OPBITEN   0x0020
#define AD9834_CTRL_SIGNPIB   0x0010
#define AD9834_CTRL_DIV2      0x0008
#define AD9834_CTRL_MODE      0x0002

/* What the latch sink's "reg" argument names. */
#define AD9834_REG_FREQ0      0
#define AD9834_REG_FREQ1      1
#define AD9834_REG_PHASE0     2
#define AD9834_REG_PHASE1     3
#define AD9834_REG_CONTROL    0xFF

typedef void (*AD9834LatchSink)(void *opaque, uint32_t ftw, unsigned reg,
                                uint32_t control);

struct AD9834State {
    SSIPeripheral parent_obj;

    uint32_t mclk_hz;

    /* The word being assembled from the two bytes the bus delivers. */
    bool selected;
    uint8_t byte_phase;
    uint16_t word;

    uint16_t control;       /* the 14 control bits */
    uint32_t freq[2];       /* 28-bit frequency tuning words */
    uint16_t phase[2];      /* 12-bit phase words */

    /* With B28 set, the first word of a pair holds the 14 LSBs. */
    bool freq_pending[2];
    uint16_t freq_lsb[2];

    AD9834LatchSink latch_sink;
    void *latch_sink_opaque;
};

/**
 * ad9834_set_latch_sink:
 *
 * Called on every completed frequency register update (reg 0 or 1, ftw the
 * new 28-bit word), on phase register writes (reg 2 or 3, ftw the phase) and
 * on control register writes (reg AD9834_REG_CONTROL, ftw the frequency word
 * currently selected by FSEL). @control is always the control register.
 */
void ad9834_set_latch_sink(AD9834State *s, AD9834LatchSink sink, void *opaque);

/** The output frequency in Hz: freq[FSEL] * mclk / 2^28. */
uint64_t ad9834_frequency_hz(AD9834State *s);

#endif /* HW_CHIPS_AD9834_H */
