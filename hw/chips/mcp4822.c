/*
 * Microchip MCP4822 dual 12-bit voltage-output DAC
 *
 * Two DACs with an internal 2.048 V reference behind a write-only SPI port
 * (modes 0,0 and 1,1, MSB first). A write is one 16-bit word under one CS
 * assertion: bit 15 picks DAC A or B, bit 13 the gain (1 = 1x, 2.048 V full
 * scale; 0 = 2x), bit 12 shutdown (1 = active), bits 11..0 the code. The word
 * reaches the channel's input register on CS's rising edge, and only if
 * exactly 16 clocks arrived; anything else is ignored, as on the part.
 *
 * The output registers load from the input registers on the falling edge of
 * LDAC (both channels at once), or on CS's rising edge while LDAC is held low.
 * Vout = 2.048 V x gain x code / 4096; a shut-down channel's output is off.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/chips/mcp4822.h"

#define MCP4822_VREF_UV 2048000u

void mcp4822_set_output_sink(MCP4822State *s, MCP4822OutputSink sink, void *opaque)
{
    s->sink = sink;
    s->sink_opaque = opaque;
}

uint32_t mcp4822_output_uv(MCP4822State *s, unsigned ch)
{
    uint64_t uv;

    ch &= 1;
    if (!s->out_active[ch]) {
        return 0;
    }
    uv = (uint64_t)MCP4822_VREF_UV * s->out_code[ch] / 4096u;
    return (uint32_t)(s->out_gain1x[ch] ? uv : 2 * uv);
}

static void mcp4822_update(MCP4822State *s, unsigned mask)
{
    for (unsigned ch = 0; ch < 2; ch++) {
        if (!(mask & (1u << ch)) || !s->in_valid[ch]) {
            continue;
        }
        s->out_code[ch] = s->in_code[ch];
        s->out_gain1x[ch] = s->in_gain1x[ch];
        s->out_active[ch] = s->in_active[ch];
    }
    s->updates++;
    /* Every load is reported, changed or not: a run of equal samples is still
       one output update per sample period. */
    if (s->sink) {
        s->sink(s->sink_opaque, s->out_code, s->out_gain1x, s->out_active);
    }
}

static uint32_t mcp4822_transfer(SSIPeripheral *dev, uint32_t val)
{
    MCP4822State *s = MCP4822(dev);

    if (s->nbytes < 2) {
        s->word = (s->word << 8) | (val & 0xFF);
    }
    if (s->nbytes < 255) {
        s->nbytes++;
    }
    /* SDI only; nothing comes back on SDO (there is no SDO pin). */
    return 0;
}

static void mcp4822_latch_word(MCP4822State *s)
{
    unsigned ch = (s->word & MCP4822_CMD_B) ? 1 : 0;

    s->in_code[ch] = s->word & MCP4822_DATA_MASK;
    s->in_gain1x[ch] = !!(s->word & MCP4822_CMD_NGA);
    s->in_active[ch] = !!(s->word & MCP4822_CMD_NSHDN);
    s->in_valid[ch] = true;
    s->frames++;
    if (!s->ldac_level || s->ldac_tied_low) {
        mcp4822_update(s, 1u << ch);
    }
}

/* @cs is the pin level: low is selected. */
static int mcp4822_set_cs(SSIPeripheral *dev, bool cs)
{
    MCP4822State *s = MCP4822(dev);

    if (!cs) {                       /* falling edge: a new word starts */
        s->selected = true;
        s->nbytes = 0;
        s->word = 0;
        return 0;
    }
    if (s->selected) {               /* rising edge: the word is written */
        if (s->nbytes == 2) {
            mcp4822_latch_word(s);
        } else if (s->nbytes) {
            s->bad_frames++;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mcp4822: CS rose after %u bytes; write ignored "
                          "(a command is exactly 16 clocks)\n", s->nbytes);
        }
    }
    s->selected = false;
    s->nbytes = 0;
    return 0;
}

static void mcp4822_ldac(void *opaque, int n, int level)
{
    MCP4822State *s = MCP4822(opaque);
    bool high = !!level;

    if (s->ldac_level && !high) {    /* falling edge: both outputs load */
        mcp4822_update(s, 3);
    }
    s->ldac_level = high;
}

static void mcp4822_get_uint32(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    MCP4822State *s = MCP4822(obj);
    uint32_t val = 0;

    if (!strcmp(name, "out-a")) {
        val = s->out_code[0];
    } else if (!strcmp(name, "out-b")) {
        val = s->out_code[1];
    } else if (!strcmp(name, "in-a")) {
        val = s->in_code[0];
    } else if (!strcmp(name, "in-b")) {
        val = s->in_code[1];
    } else if (!strcmp(name, "frames")) {
        val = s->frames;
    } else if (!strcmp(name, "bad-frames")) {
        val = s->bad_frames;
    } else if (!strcmp(name, "updates")) {
        val = s->updates;
    } else if (!strcmp(name, "out-a-uv")) {
        val = mcp4822_output_uv(s, 0);
    } else if (!strcmp(name, "out-b-uv")) {
        val = mcp4822_output_uv(s, 1);
    }
    visit_type_uint32(v, name, &val, errp);
}

static void mcp4822_reset_hold(Object *obj, ResetType type)
{
    MCP4822State *s = MCP4822(obj);

    /* Power-on reset: both channels shut down, registers at zero. */
    s->selected = false;
    s->nbytes = 0;
    s->word = 0;
    for (unsigned ch = 0; ch < 2; ch++) {
        s->in_code[ch] = s->out_code[ch] = 0;
        s->in_gain1x[ch] = s->out_gain1x[ch] = true;
        s->in_active[ch] = s->out_active[ch] = false;
        s->in_valid[ch] = false;
    }
    s->ldac_level = true;
    s->frames = s->bad_frames = s->updates = 0;
}

static void mcp4822_realize(SSIPeripheral *dev, Error **errp)
{
    static const char *const props[] = {
        "out-a", "out-b", "in-a", "in-b", "frames", "bad-frames", "updates",
        "out-a-uv", "out-b-uv",
    };
    Object *obj = OBJECT(dev);

    qdev_init_gpio_in_named(DEVICE(dev), mcp4822_ldac, MCP4822_LDAC_GPIO, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(props); i++) {
        object_property_add(obj, props[i], "uint32", mcp4822_get_uint32,
                            NULL, NULL, NULL);
    }
}

static const Property mcp4822_properties[] = {
    DEFINE_PROP_BOOL("ldac-tied-low", MCP4822State, ldac_tied_low, false),
};

static const VMStateDescription mcp4822_vmstate = {
    .name = "mcp4822",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(selected, MCP4822State),
        VMSTATE_UINT8(nbytes, MCP4822State),
        VMSTATE_UINT16(word, MCP4822State),
        VMSTATE_UINT16_ARRAY(in_code, MCP4822State, 2),
        VMSTATE_BOOL_ARRAY(in_gain1x, MCP4822State, 2),
        VMSTATE_BOOL_ARRAY(in_active, MCP4822State, 2),
        VMSTATE_BOOL_ARRAY(in_valid, MCP4822State, 2),
        VMSTATE_UINT16_ARRAY(out_code, MCP4822State, 2),
        VMSTATE_BOOL_ARRAY(out_gain1x, MCP4822State, 2),
        VMSTATE_BOOL_ARRAY(out_active, MCP4822State, 2),
        VMSTATE_BOOL(ldac_level, MCP4822State),
        VMSTATE_UINT32(frames, MCP4822State),
        VMSTATE_UINT32(bad_frames, MCP4822State),
        VMSTATE_UINT32(updates, MCP4822State),
        VMSTATE_END_OF_LIST()
    }
};

static void mcp4822_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    sc->realize = mcp4822_realize;
    sc->transfer = mcp4822_transfer;
    sc->set_cs = mcp4822_set_cs;
    sc->cs_polarity = SSI_CS_LOW;
    dc->vmsd = &mcp4822_vmstate;
    device_class_set_props(dc, mcp4822_properties);
    rc->phases.hold = mcp4822_reset_hold;
}

static const TypeInfo mcp4822_types[] = {
    {
        .name = TYPE_MCP4822,
        .parent = TYPE_SSI_PERIPHERAL,
        .instance_size = sizeof(MCP4822State),
        .class_init = mcp4822_class_init,
    },
};

DEFINE_TYPES(mcp4822_types)
