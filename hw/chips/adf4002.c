/*
 * Analog Devices ADF4002 Phase-Locked Loop (PLL) Synthesizer
 *
 * Implements the 24-bit SPI latching interface: Function latch (0b10),
 * R-counter latch (0b00), N-counter latch (0b01) and Initialization latch (0b11).
 * Bytes clock into a 24-bit shift register; the word is transferred to the
 * addressed latch on the rising edge of LE, as on the real part. When
 * programmed with valid R and N counters and normal operation, and with
 * MUXOUT selected for digital lock detect, the MUXOUT line is driven high.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "hw/chips/adf4002.h"

/*
 * MUXOUT carries digital lock detect only when M3,M2,M1 = 0,0,1 (DB6:DB4 of
 * the Function Latch). The other MUXOUT selections (divider outputs,
 * three-state, DVDD/DGND, serial data) are not modelled, so for them the pin
 * reads low.
 */
static bool adf4002_muxout_lock_detect(ADF4002State *s)
{
    return ((s->func_latch >> 4) & 0x07) == 0x01;
}

static void adf4002_update_lock(ADF4002State *s)
{
    /*
     * Lock requires CE enabled, valid R and N counter latches, and the
     * counter reset bit (F1, bit 2 of Function Latch) cleared.
     */
    bool counter_reset = (s->func_latch & 0x04) != 0;
    bool pin;

    s->locked = s->ce && (s->r_counter > 0) && (s->n_counter > 0) && !counter_reset;

    pin = s->locked && adf4002_muxout_lock_detect(s);
    if (pin != s->mux_out_level) {
        s->mux_out_level = pin;
        qemu_set_irq(s->mux_out, pin ? 1 : 0);
    }
    if (s->latch_sink) {
        s->latch_sink(s->latch_sink_opaque, s->r_counter, s->n_counter,
                      s->func_latch, s->locked);
    }
}

void adf4002_set_latch_sink(ADF4002State *s,
                            void (*sink)(void *opaque, uint16_t r, uint16_t n, uint32_t func, bool locked),
                            void *opaque)
{
    s->latch_sink = sink;
    s->latch_sink_opaque = opaque;
}

static void adf4002_apply_latch(ADF4002State *s)
{
    uint8_t control_bits = s->shift_reg & 0x03;

    switch (control_bits) {
    case 0x00: /* R counter */
        s->r_latch = s->shift_reg;
        s->r_counter = (s->shift_reg >> 2) & 0x3FFF;
        break;
    case 0x01: /* N counter */
        s->n_latch = s->shift_reg;
        s->n_counter = (s->shift_reg >> 8) & 0x1FFF;
        break;
    case 0x02: /* Function latch */
        s->func_latch = s->shift_reg;
        break;
    case 0x03: /* Initialization latch */
        s->init_latch = s->shift_reg;
        break;
    }
    adf4002_update_lock(s);
}

static uint32_t adf4002_transfer(SSIPeripheral *dev, uint32_t val)
{
    ADF4002State *s = ADF4002(dev);

    /*
     * Bytes only shift in here; the last 24 bits are transferred to a latch
     * on the rising edge of LE (adf4002_set_le), which is how the part
     * behaves -- the serial clock never latches on its own.
     */
    s->shift_reg = ((s->shift_reg << 8) | (val & 0xFF)) & 0xFFFFFF;
    return 0;
}

static void adf4002_set_le(void *opaque, int line, int level)
{
    ADF4002State *s = opaque;
    bool rising = !s->le && level;
    s->le = level != 0;

    if (rising) {
        adf4002_apply_latch(s);
    }
}

static void adf4002_set_ce(void *opaque, int line, int level)
{
    ADF4002State *s = opaque;
    s->ce = level != 0;
    adf4002_update_lock(s);
}

static void adf4002_get_uint32(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    ADF4002State *s = ADF4002(obj);
    uint32_t val = 0;

    if (!strcmp(name, "r-counter")) {
        val = s->r_counter;
    } else if (!strcmp(name, "n-counter")) {
        val = s->n_counter;
    }
    visit_type_uint32(v, name, &val, errp);
}

static void adf4002_get_uint64(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    ADF4002State *s = ADF4002(obj);
    uint64_t val = 0;

    if (!strcmp(name, "frequency-lo-hz")) {
        if (s->r_counter > 0) {
            val = (uint64_t)s->n_counter * (8000000ULL / s->r_counter);
        }
    }
    visit_type_uint64(v, name, &val, errp);
}

static void adf4002_get_bool(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    ADF4002State *s = ADF4002(obj);
    bool val = s->locked;
    visit_type_bool(v, name, &val, errp);
}

static void adf4002_reset_hold(Object *obj, ResetType type)
{
    ADF4002State *s = ADF4002(obj);

    s->shift_reg = 0;
    s->r_latch = 0;
    s->n_latch = 0;
    s->func_latch = 0;
    s->init_latch = 0;
    s->r_counter = 0;
    s->n_counter = 0;
    s->ce = false;
    s->le = false;
    s->locked = false;
    s->mux_out_level = false;
    qemu_set_irq(s->mux_out, 0);
}

static void adf4002_realize(SSIPeripheral *dev, Error **errp)
{
    ADF4002State *s = ADF4002(dev);
    DeviceState *d = DEVICE(dev);

    qdev_init_gpio_out_named(d, &s->mux_out, ADF4002_MUX_OUT_GPIO, 1);
    qdev_init_gpio_in_named(d, adf4002_set_le, ADF4002_LE_GPIO, 1);
    qdev_init_gpio_in_named(d, adf4002_set_ce, ADF4002_CE_GPIO, 1);

    object_property_add(OBJECT(dev), "r-counter", "uint32",
                        adf4002_get_uint32, NULL, NULL, NULL);
    object_property_add(OBJECT(dev), "n-counter", "uint32",
                        adf4002_get_uint32, NULL, NULL, NULL);
    object_property_add(OBJECT(dev), "frequency-lo-hz", "uint64",
                        adf4002_get_uint64, NULL, NULL, NULL);
    object_property_add(OBJECT(dev), "locked", "bool",
                        adf4002_get_bool, NULL, NULL, NULL);
}

static const VMStateDescription adf4002_vmstate = {
    .name = "adf4002",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(shift_reg, ADF4002State),
        VMSTATE_UINT32(r_latch, ADF4002State),
        VMSTATE_UINT32(n_latch, ADF4002State),
        VMSTATE_UINT32(func_latch, ADF4002State),
        VMSTATE_UINT32(init_latch, ADF4002State),
        VMSTATE_UINT16(r_counter, ADF4002State),
        VMSTATE_UINT16(n_counter, ADF4002State),
        VMSTATE_BOOL(ce, ADF4002State),
        VMSTATE_BOOL(le, ADF4002State),
        VMSTATE_BOOL(locked, ADF4002State),
        VMSTATE_BOOL(mux_out_level, ADF4002State),
        VMSTATE_END_OF_LIST()
    }
};

static void adf4002_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    sc->realize = adf4002_realize;
    sc->transfer = adf4002_transfer;
    sc->cs_polarity = SSI_CS_LOW;
    dc->vmsd = &adf4002_vmstate;
    rc->phases.hold = adf4002_reset_hold;
}

static const TypeInfo adf4002_types[] = {
    {
        .name = TYPE_ADF4002,
        .parent = TYPE_SSI_PERIPHERAL,
        .instance_size = sizeof(ADF4002State),
        .class_init = adf4002_class_init,
    },
};

DEFINE_TYPES(adf4002_types)
