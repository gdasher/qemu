/*
 * PIC16 Timer0 (8-bit / 16-bit Timer with Period Match)
 *
 * Runs on QEMU_CLOCK_VIRTUAL via ptimer. In 8-bit mode (the common case and the
 * mode used by modern PIC16 audio/modulation code), TMR0H acts as the period
 * register (PR0). Each match reloads TMR0L to 0, advances the postscaler, and
 * pulses TMR0IF.
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
    default:
        return fosc_hz;
    }
}

static uint32_t pic16_tmr0_prescale(PIC16Tmr0State *s)
{
    unsigned ckps = s->con1 & T0CON1_T0CKPS_MASK;
    return 1u << ckps;
}

static uint32_t pic16_tmr0_postscale(PIC16Tmr0State *s)
{
    return (s->con0 & 0x0F) + 1;
}

static void pic16_tmr0_rearm(PIC16Tmr0State *s)
{
    bool enabled = (s->con0 & (1u << T0CON0_T0EN)) != 0;
    uint64_t clk_hz = pic16_tmr0_clk_hz(s);
    uint32_t prescale = pic16_tmr0_prescale(s);
    uint32_t postscale = pic16_tmr0_postscale(s);
    uint64_t tick_hz = clk_hz / prescale;
    uint32_t limit;

    if (tick_hz == 0) {
        tick_hz = 1;
    }

    if (s->con0 & (1u << T0CON0_T016BIT)) {
        limit = 0x10000u * postscale;
    } else {
        limit = (s->tmr0h + 1u) * postscale;
    }

    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, tick_hz);
    ptimer_set_limit(s->timer, limit, 1);
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

    s->tmr0l = 0;
    qemu_set_irq(s->irq, 1);
    qemu_set_irq(s->irq, 0);
}

static uint64_t pic16_tmr0_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16Tmr0State *s = opaque;

    switch (addr) {
    case REG_TMR0L:
        if (s->con0 & (1u << T0CON0_T0EN)) {
            uint32_t postscale = pic16_tmr0_postscale(s);
            uint64_t count = ptimer_get_count(s->timer) / postscale;
            if (s->con0 & (1u << T0CON0_T016BIT)) {
                uint32_t total = 0x10000u - count;
                s->tmr0h_buf = (total >> 8) & 0xFF;
                return total & 0xFF;
            } else {
                return (s->tmr0h + 1u - count) & 0xFF;
            }
        }
        return s->tmr0l;
    case REG_TMR0H:
        if (s->con0 & (1u << T0CON0_T016BIT)) {
            return s->tmr0h_buf;
        }
        return s->tmr0h;
    case REG_T0CON0:
        return s->con0;
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

    switch (addr) {
    case REG_TMR0L:
        s->tmr0l = value;
        pic16_tmr0_rearm(s);
        break;
    case REG_TMR0H:
        if (s->con0 & (1u << T0CON0_T016BIT)) {
            s->tmr0h_buf = value;
        } else {
            s->tmr0h = value;
            pic16_tmr0_rearm(s);
        }
        break;
    case REG_T0CON0:
        s->con0 = value;
        pic16_tmr0_rearm(s);
        break;
    case REG_T0CON1:
        s->con1 = value;
        pic16_tmr0_rearm(s);
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

    s->tmr0l = 0;
    s->tmr0h = 0xFF;
    s->tmr0h_buf = 0;
    s->con0 = 0;
    s->con1 = 0;
    pic16_tmr0_rearm(s);
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
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(timer, PIC16Tmr0State),
        VMSTATE_UINT8(tmr0l, PIC16Tmr0State),
        VMSTATE_UINT8(tmr0h, PIC16Tmr0State),
        VMSTATE_UINT8(tmr0h_buf, PIC16Tmr0State),
        VMSTATE_UINT8(con0, PIC16Tmr0State),
        VMSTATE_UINT8(con1, PIC16Tmr0State),
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
