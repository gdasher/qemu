/*
 * PIC16 Timer1
 *
 * A 16-bit up-counter that raises TMR1IF when it rolls over. ptimer counts
 * down, so the counter value is kept as the distance from the rollover: TMR1
 * reads back as 0x10000 minus the remaining count.
 *
 * The timer runs on QEMU_CLOCK_VIRTUAL, so its rate stays in proportion to
 * instruction execution under -icount. That matters for the reference
 * firmware, whose motion state machine is driven entirely from this
 * interrupt while the main loop runs at a nominal 8 MIPS.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "pic16_tmr1.h"

enum {
    REG_TMR1L,
    REG_TMR1H,
    REG_T1CON,
    REG_T1GCON,
    REG_T1GATE,
    REG_T1CLK,
    PIC16_TMR1_NREGS,
};

/* Bit positions, confirmed against the compiled firmware's StepperInit(). */
#define T1CON_ON    0
#define T1CON_CKPS_SHIFT 4
#define T1CON_CKPS_MASK  0x03

/* T1CLK source selection; only the two internal sources are modelled. */
#define T1CLK_FOSC4 0x01
#define T1CLK_FOSC  0x02

#define TMR1_PERIOD 0x10000

static uint32_t pic16_tmr1_freq(PIC16Tmr1State *s)
{
    uint32_t base = clock_get_hz(s->fosc);
    unsigned prescale = 1u << ((s->con >> T1CON_CKPS_SHIFT) & T1CON_CKPS_MASK);

    switch (s->clk & 0x0F) {
    case T1CLK_FOSC:
        break;
    case T1CLK_FOSC4:
        base /= 4;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "pic16-tmr1: clock source 0x%02x is not modelled, "
                      "using Fosc/4\n", s->clk & 0x0F);
        base /= 4;
        break;
    }
    return base / prescale;
}

static uint16_t pic16_tmr1_count(PIC16Tmr1State *s)
{
    return TMR1_PERIOD - (ptimer_get_count(s->timer) & 0xFFFF);
}

static void pic16_tmr1_reload(PIC16Tmr1State *s, uint16_t value)
{
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, pic16_tmr1_freq(s));
    ptimer_set_limit(s->timer, TMR1_PERIOD, 0);
    ptimer_set_count(s->timer, TMR1_PERIOD - value);
    if (s->con & (1u << T1CON_ON)) {
        ptimer_run(s->timer, 0);
    } else {
        ptimer_stop(s->timer);
    }
    ptimer_transaction_commit(s->timer);
}

static void pic16_tmr1_tick(void *opaque)
{
    PIC16Tmr1State *s = opaque;

    /* TMR1IF latches; the handler clears it by writing PIR1. */
    qemu_irq_pulse(s->irq);
}

static uint64_t pic16_tmr1_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16Tmr1State *s = opaque;
    uint16_t count;

    switch (addr) {
    case REG_TMR1L:
        count = pic16_tmr1_count(s);
        s->high = count >> 8;   /* buffer the high byte for the next read */
        return count & 0xFF;
    case REG_TMR1H:
        return s->high;
    case REG_T1CON:
        return s->con;
    case REG_T1GCON:
        return s->gcon;
    case REG_T1GATE:
        return s->gate;
    case REG_T1CLK:
        return s->clk;
    default:
        return 0;
    }
}

static void pic16_tmr1_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    PIC16Tmr1State *s = opaque;

    switch (addr) {
    case REG_TMR1L:
        /* The buffered high byte is written along with the low one. */
        pic16_tmr1_reload(s, (s->high << 8) | (value & 0xFF));
        break;
    case REG_TMR1H:
        s->high = value;
        break;
    case REG_T1CON:
        s->con = value;
        pic16_tmr1_reload(s, pic16_tmr1_count(s));
        break;
    case REG_T1GCON:
        s->gcon = value;
        if (value & 0x80) {
            qemu_log_mask(LOG_UNIMP, "pic16-tmr1: gate control ignored\n");
        }
        break;
    case REG_T1GATE:
        s->gate = value;
        break;
    case REG_T1CLK:
        s->clk = value;
        pic16_tmr1_reload(s, pic16_tmr1_count(s));
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pic16_tmr1_ops = {
    .read = pic16_tmr1_read,
    .write = pic16_tmr1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic16_tmr1_reset_hold(Object *obj, ResetType type)
{
    PIC16Tmr1State *s = PIC16_TMR1(obj);

    s->con = 0;
    s->gcon = 0;
    s->gate = 0;
    s->clk = 0;
    s->high = 0;

    ptimer_transaction_begin(s->timer);
    ptimer_stop(s->timer);
    ptimer_set_limit(s->timer, TMR1_PERIOD, 1);
    ptimer_transaction_commit(s->timer);
}

static void pic16_tmr1_realize(DeviceState *dev, Error **errp)
{
    PIC16Tmr1State *s = PIC16_TMR1(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->timer = ptimer_init(pic16_tmr1_tick, s, PTIMER_POLICY_LEGACY);

    memory_region_init_io(&s->iomem, OBJECT(dev), &pic16_tmr1_ops, s,
                          "pic16.tmr1", PIC16_TMR1_NREGS);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription pic16_tmr1_vmstate = {
    .name = "pic16-tmr1",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(timer, PIC16Tmr1State),
        VMSTATE_UINT8(con, PIC16Tmr1State),
        VMSTATE_UINT8(gcon, PIC16Tmr1State),
        VMSTATE_UINT8(gate, PIC16Tmr1State),
        VMSTATE_UINT8(clk, PIC16Tmr1State),
        VMSTATE_UINT8(high, PIC16Tmr1State),
        VMSTATE_END_OF_LIST()
    }
};

static void pic16_tmr1_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic16_tmr1_realize;
    dc->vmsd = &pic16_tmr1_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = pic16_tmr1_reset_hold;
}

static const TypeInfo pic16_tmr1_types[] = {
    {
        .name = TYPE_PIC16_TMR1,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC16Tmr1State),
        .class_init = pic16_tmr1_class_init,
    },
};

DEFINE_TYPES(pic16_tmr1_types)
