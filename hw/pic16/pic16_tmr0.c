/*
 * PIC16 Timer0 (8-bit / 16-bit Timer with Period Match)
 *
 * Runs on QEMU_CLOCK_VIRTUAL via ptimer. In 8-bit mode (the common case and the
 * mode used by modern PIC16 audio/modulation code), TMR0H acts as the period
 * register (PR0): the count runs 0..PR0 and wraps. In 16-bit mode the count
 * runs the full 0..FFFF. Each wrap advances the postscaler, and every T0OUTPS
 * wraps toggle T0OUT and pulse TMR0IF.
 *
 * The ptimer counts one period down; the value read back as TMR0L is the
 * distance into that period, and writing TMR0L moves the count.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "pic16_tmr0.h"

#define T0CON0_T0EN    7
#define T0CON0_T0OUT   5
#define T0CON0_T016BIT 4
#define T0CON0_T0OUTPS_MASK 0x0F

#define T0CON1_T0CS_SHIFT  5
#define T0CON1_T0CS_MASK   0x07
#define T0CON1_T0ASYNC     4
#define T0CON1_T0CKPS_MASK 0x0F

static uint64_t pic16_tmr0_clk_hz(PIC16Tmr0State *s)
{
    uint64_t fosc_hz = s->fosc ? clock_get_hz(s->fosc) : 32000000;
    unsigned cs = (s->con1 >> T0CON1_T0CS_SHIFT) & T0CON1_T0CS_MASK;

    switch (cs) {
    case 2: /* FOSC / 4 */
        return fosc_hz / 4;
    case 4: /* LFINTOSC */
        return 31000;
    case 5: /* MFINTOSC (500 kHz) */
        return 500000;
    case 3: /* HFINTOSC */
        return fosc_hz;
    default:
        /*
         * T0CKIPPS, SOSC and CLC1 are pins and modules this model does not
         * clock from; the timer holds.
         */
        return 0;
    }
}

static bool pic16_tmr0_is_16bit(PIC16Tmr0State *s)
{
    return (s->con0 & (1u << T0CON0_T016BIT)) != 0;
}

/* Ticks in one wrap of the counter. */
static uint32_t pic16_tmr0_period(PIC16Tmr0State *s)
{
    return pic16_tmr0_is_16bit(s) ? 0x10000u : s->tmr0h + 1u;
}

static uint32_t pic16_tmr0_postscale(PIC16Tmr0State *s)
{
    return (s->con0 & T0CON0_T0OUTPS_MASK) + 1;
}

/* Where the count is within the current period. */
static uint32_t pic16_tmr0_count(PIC16Tmr0State *s)
{
    uint32_t period = pic16_tmr0_period(s);
    uint64_t remaining = ptimer_get_count(s->timer);

    return remaining >= period ? 0 : period - remaining;
}

static void pic16_tmr0_rearm(PIC16Tmr0State *s, uint32_t count)
{
    bool enabled = (s->con0 & (1u << T0CON0_T0EN)) != 0;
    uint64_t clk_hz = pic16_tmr0_clk_hz(s);
    uint32_t prescale = 1u << (s->con1 & T0CON1_T0CKPS_MASK);
    uint64_t tick_hz = clk_hz / prescale;
    uint32_t period = pic16_tmr0_period(s);

    ptimer_transaction_begin(s->timer);
    if (tick_hz == 0) {
        enabled = false;
        tick_hz = 1;
    }
    ptimer_set_freq(s->timer, tick_hz);
    ptimer_set_limit(s->timer, period, 0);
    ptimer_set_count(s->timer, period - (count % period));
    if (enabled) {
        ptimer_run(s->timer, 0);
    } else {
        ptimer_stop(s->timer);
    }
    ptimer_transaction_commit(s->timer);
}

static void pic16_tmr0_expire(void *opaque)
{
    PIC16Tmr0State *s = opaque;

    if (++s->postcount < pic16_tmr0_postscale(s)) {
        return;
    }
    s->postcount = 0;
    s->t0out = !s->t0out;
    qemu_set_irq(s->irq, 1);
    qemu_set_irq(s->irq, 0);
}

static uint64_t pic16_tmr0_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16Tmr0State *s = opaque;
    uint32_t count;

    switch (addr) {
    case REG_TMR0L:
        count = pic16_tmr0_count(s);
        if (pic16_tmr0_is_16bit(s)) {
            /* Reading the low byte latches the high one. */
            s->tmr0h_buf = (count >> 8) & 0xFF;
        }
        return count & 0xFF;
    case REG_TMR0H:
        return pic16_tmr0_is_16bit(s) ? s->tmr0h_buf : s->tmr0h;
    case REG_T0CON0:
        return (s->con0 & ~(1u << T0CON0_T0OUT)) |
               (s->t0out ? 1u << T0CON0_T0OUT : 0);
    case REG_T0CON1:
        return s->con1;
    default:
        return 0;
    }
}

static void pic16_tmr0_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    PIC16Tmr0State *s = opaque;
    uint32_t count = pic16_tmr0_count(s);

    switch (addr) {
    case REG_TMR0L:
        if (pic16_tmr0_is_16bit(s)) {
            /* Writing the low byte loads the buffered high one with it. */
            count = ((uint32_t)s->tmr0h_buf << 8) | (value & 0xFF);
        } else {
            count = value & 0xFF;
        }
        pic16_tmr0_rearm(s, count);
        break;
    case REG_TMR0H:
        if (pic16_tmr0_is_16bit(s)) {
            s->tmr0h_buf = value;
        } else {
            s->tmr0h = value;
            pic16_tmr0_rearm(s, count);
        }
        break;
    case REG_T0CON0:
        s->con0 = value & ~(1u << T0CON0_T0OUT);
        pic16_tmr0_rearm(s, count);
        break;
    case REG_T0CON1:
        s->con1 = value;
        pic16_tmr0_rearm(s, count);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pic16_tmr0_ops = {
    .read = pic16_tmr0_read,
    .write = pic16_tmr0_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic16_tmr0_reset_hold(Object *obj, ResetType type)
{
    PIC16Tmr0State *s = PIC16_TMR0(obj);

    s->tmr0h = 0xFF;
    s->tmr0h_buf = 0;
    s->con0 = 0;
    s->con1 = 0;
    s->postcount = 0;
    s->t0out = false;
    pic16_tmr0_rearm(s, 0);
}

void pic16_tmr0_update_fosc(PIC16Tmr0State *s)
{
    uint32_t count = pic16_tmr0_count(s);
    pic16_tmr0_rearm(s, count);
}

static void pic16_tmr0_realize(DeviceState *dev, Error **errp)
{
    PIC16Tmr0State *s = PIC16_TMR0(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->timer = ptimer_init(pic16_tmr0_expire, s, PTIMER_POLICY_LEGACY);

    memory_region_init_io(&s->iomem, OBJECT(dev), &pic16_tmr0_ops, s,
                          "pic16.tmr0", PIC16_TMR0_NREGS);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription pic16_tmr0_vmstate = {
    .name = "pic16-tmr0",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(timer, PIC16Tmr0State),
        VMSTATE_UINT8(tmr0h, PIC16Tmr0State),
        VMSTATE_UINT8(tmr0h_buf, PIC16Tmr0State),
        VMSTATE_UINT8(con0, PIC16Tmr0State),
        VMSTATE_UINT8(con1, PIC16Tmr0State),
        VMSTATE_UINT8(postcount, PIC16Tmr0State),
        VMSTATE_BOOL(t0out, PIC16Tmr0State),
        VMSTATE_END_OF_LIST()
    }
};

static void pic16_tmr0_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic16_tmr0_realize;
    dc->vmsd = &pic16_tmr0_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = pic16_tmr0_reset_hold;
}

static const TypeInfo pic16_tmr0_types[] = {
    {
        .name = TYPE_PIC16_TMR0,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC16Tmr0State),
        .class_init = pic16_tmr0_class_init,
    },
};

DEFINE_TYPES(pic16_tmr0_types)
