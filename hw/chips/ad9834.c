/*
 * Analog Devices AD9834 direct digital synthesizer
 *
 * A 28-bit DDS programmed over a three-wire serial port with 16-bit words,
 * MSB first, framed by FSYNC (low for the whole word, or for a run of words).
 * D15:D14 address the word: 00 the control register, 01 FREQ0, 10 FREQ1,
 * 11 a phase register (D13 picks PHASE0/PHASE1). A frequency register is 28
 * bits wide and takes two 14-bit words: with B28 set the LSBs then the MSBs,
 * the register updating when the second arrives; with B28 clear a single word
 * writes the half HLB selects. The output is freq[FSEL] * MCLK / 2^28.
 *
 * FSYNC is the SSI chip select. The bytes the bus delivers pair up into
 * words while the chip is selected; a deselect between the two bytes drops
 * the half-word, which is what the part does too.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/chips/ad9834.h"

#define AD9834_FTW_BITS 28
#define AD9834_FTW_MASK ((1u << AD9834_FTW_BITS) - 1)
#define AD9834_HALF_MASK 0x3FFF

void ad9834_set_latch_sink(AD9834State *s, AD9834LatchSink sink, void *opaque)
{
    s->latch_sink = sink;
    s->latch_sink_opaque = opaque;
}

static unsigned ad9834_fsel(AD9834State *s)
{
    return (s->control & AD9834_CTRL_FSEL) ? 1 : 0;
}

uint64_t ad9834_frequency_hz(AD9834State *s)
{
    return ((uint64_t)s->freq[ad9834_fsel(s)] * s->mclk_hz) >> AD9834_FTW_BITS;
}

static void ad9834_notify(AD9834State *s, unsigned reg, uint32_t value)
{
    if (s->latch_sink) {
        s->latch_sink(s->latch_sink_opaque, value, reg, s->control);
    }
}

static void ad9834_write_freq(AD9834State *s, unsigned n, uint16_t data)
{
    data &= AD9834_HALF_MASK;

    if (s->control & AD9834_CTRL_B28) {
        if (!s->freq_pending[n]) {
            s->freq_lsb[n] = data;
            s->freq_pending[n] = true;
            return;
        }
        s->freq[n] = ((uint32_t)data << 14) | s->freq_lsb[n];
        s->freq_pending[n] = false;
    } else if (s->control & AD9834_CTRL_HLB) {
        s->freq[n] = (s->freq[n] & AD9834_HALF_MASK) | ((uint32_t)data << 14);
    } else {
        s->freq[n] = (s->freq[n] & ~(uint32_t)AD9834_HALF_MASK) | data;
    }
    s->freq[n] &= AD9834_FTW_MASK;
    ad9834_notify(s, n, s->freq[n]);
}

static void ad9834_write_word(AD9834State *s, uint16_t word)
{
    switch (word >> 14) {
    case 0:
        s->control = word & 0x3FFF;
        if (!(s->control & AD9834_CTRL_B28)) {
            /* A half-written pair does not carry across the mode change. */
            s->freq_pending[0] = false;
            s->freq_pending[1] = false;
        }
        ad9834_notify(s, AD9834_REG_CONTROL, s->freq[ad9834_fsel(s)]);
        break;
    case 1:
        ad9834_write_freq(s, 0, word);
        break;
    case 2:
        ad9834_write_freq(s, 1, word);
        break;
    default: {
        unsigned n = (word >> 13) & 1;

        s->phase[n] = word & 0x0FFF;
        ad9834_notify(s, AD9834_REG_PHASE0 + n, s->phase[n]);
        break;
    }
    }
}

static uint32_t ad9834_transfer(SSIPeripheral *dev, uint32_t val)
{
    AD9834State *s = AD9834(dev);

    s->word = (s->word << 8) | (val & 0xFF);
    if (++s->byte_phase == 2) {
        s->byte_phase = 0;
        ad9834_write_word(s, s->word);
    }
    /* SDATA is input only; nothing comes back. */
    return 0;
}

/* @cs is the pin level: low is selected. */
static int ad9834_set_cs(SSIPeripheral *dev, bool cs)
{
    AD9834State *s = AD9834(dev);

    s->selected = !cs;
    if (s->byte_phase) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ad9834: FSYNC released after an odd byte; half-word "
                      "dropped\n");
    }
    s->byte_phase = 0;
    s->word = 0;
    return 0;
}

static void ad9834_get_uint32(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    AD9834State *s = AD9834(obj);
    uint32_t val = 0;

    if (!strcmp(name, "freq0")) {
        val = s->freq[0];
    } else if (!strcmp(name, "freq1")) {
        val = s->freq[1];
    } else if (!strcmp(name, "phase0")) {
        val = s->phase[0];
    } else if (!strcmp(name, "phase1")) {
        val = s->phase[1];
    } else if (!strcmp(name, "control")) {
        val = s->control;
    }
    visit_type_uint32(v, name, &val, errp);
}

static void ad9834_get_frequency(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    AD9834State *s = AD9834(obj);
    uint64_t val = ad9834_frequency_hz(s);

    visit_type_uint64(v, name, &val, errp);
}

static void ad9834_reset_hold(Object *obj, ResetType type)
{
    AD9834State *s = AD9834(obj);

    s->byte_phase = 0;
    s->word = 0;
    s->control = 0;
    s->freq[0] = s->freq[1] = 0;
    s->phase[0] = s->phase[1] = 0;
    s->freq_pending[0] = s->freq_pending[1] = false;
    s->freq_lsb[0] = s->freq_lsb[1] = 0;
}

static void ad9834_realize(SSIPeripheral *dev, Error **errp)
{
    static const char *const regs[] = {
        "freq0", "freq1", "phase0", "phase1", "control",
    };
    Object *obj = OBJECT(dev);

    if (!AD9834(dev)->mclk_hz) {
        error_setg(errp, "ad9834: mclk-hz must be nonzero");
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
        object_property_add(obj, regs[i], "uint32", ad9834_get_uint32,
                            NULL, NULL, NULL);
    }
    object_property_add(obj, "frequency-hz", "uint64", ad9834_get_frequency,
                        NULL, NULL, NULL);
}

static const Property ad9834_properties[] = {
    DEFINE_PROP_UINT32("mclk-hz", AD9834State, mclk_hz, 64000000),
};

static const VMStateDescription ad9834_vmstate = {
    .name = "ad9834",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(selected, AD9834State),
        VMSTATE_UINT8(byte_phase, AD9834State),
        VMSTATE_UINT16(word, AD9834State),
        VMSTATE_UINT16(control, AD9834State),
        VMSTATE_UINT32_ARRAY(freq, AD9834State, 2),
        VMSTATE_UINT16_ARRAY(phase, AD9834State, 2),
        VMSTATE_BOOL_ARRAY(freq_pending, AD9834State, 2),
        VMSTATE_UINT16_ARRAY(freq_lsb, AD9834State, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void ad9834_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    sc->realize = ad9834_realize;
    sc->transfer = ad9834_transfer;
    sc->set_cs = ad9834_set_cs;
    sc->cs_polarity = SSI_CS_LOW;
    dc->vmsd = &ad9834_vmstate;
    device_class_set_props(dc, ad9834_properties);
    rc->phases.hold = ad9834_reset_hold;
}

static const TypeInfo ad9834_types[] = {
    {
        .name = TYPE_AD9834,
        .parent = TYPE_SSI_PERIPHERAL,
        .instance_size = sizeof(AD9834State),
        .class_init = ad9834_class_init,
    },
};

DEFINE_TYPES(ad9834_types)
