/*
 * PIC32 timers
 *
 * A 16-bit counter that runs from the peripheral bus clock through a
 * prescaler, rolls over when it reaches PRx, and raises its interrupt when it
 * does. That is all the firmware uses: FreeRTOS's tick comes from Timer1 and
 * Harmony's time service from Timer2, both of them free-running with a period
 * and an interrupt.
 *
 * The ptimer runs on the virtual clock, so under -icount the tick rate stays
 * in proportion to how fast the guest executes rather than to the host.
 * Without that a 1 kHz tick against an emulated 120 MHz core would arrive at
 * whatever ratio the host happened to manage, and every timeout in the guest
 * would be wrong by it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/pic32/pic32_regs.h"
#include "hw/pic32/pic32_timer.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

enum {
    R_CON = 0x00,
    R_TMR = 0x10,
    R_PR  = 0x20,
};

/* TxCON */
#define CON_TCS    (1u << 1)
#define CON_T32    (1u << 3)
#define CON_TCKPS_SHIFT 4
#define CON_TGATE  (1u << 7)
#define CON_ON     (1u << 15)

/* The counter is 16 bits, so a period of PRx counts PRx + 1 ticks. */
#define TIMER_PERIOD 0x10000

static unsigned pic32_timer_prescale(PIC32TimerState *s)
{
    unsigned sel = (s->con >> CON_TCKPS_SHIFT) & (s->type_a ? 0x3 : 0x7);

    if (s->type_a) {
        /* 1:1, 1:8, 1:64, 1:256. */
        static const unsigned type_a[] = { 1, 8, 64, 256 };

        return type_a[sel];
    }
    /* 1:1 through 1:64 by powers of two, then 1:256. */
    return sel == 7 ? 256 : 1u << sel;
}

static void pic32_timer_reload(PIC32TimerState *s, bool restart)
{
    uint32_t limit = (s->pr & 0xFFFF) + 1;

    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, clock_get_hz(s->pbclk) /
                              pic32_timer_prescale(s));
    ptimer_set_limit(s->timer, limit, restart);
    if (s->con & CON_ON) {
        ptimer_run(s->timer, 0);
    } else {
        ptimer_stop(s->timer);
    }
    ptimer_transaction_commit(s->timer);
}

static void pic32_timer_tick(void *opaque)
{
    PIC32TimerState *s = opaque;

    qemu_irq_pulse(s->irq);
}

static uint32_t pic32_timer_read(void *opaque, hwaddr addr)
{
    PIC32TimerState *s = opaque;
    uint32_t limit;

    switch (addr) {
    case R_CON:
        return s->con;
    case R_TMR:
        /*
         * ptimer counts down to zero and the guest's counter counts up to PRx,
         * so what it has left is what the guest has not reached yet.
         */
        limit = (s->pr & 0xFFFF) + 1;
        return (limit - ptimer_get_count(s->timer)) & 0xFFFF;
    case R_PR:
        return s->pr;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-timer: read of 0x%02x\n",
                      (unsigned)addr);
        return 0;
    }
}

static void pic32_timer_write(void *opaque, hwaddr addr, uint32_t value)
{
    PIC32TimerState *s = opaque;

    switch (addr) {
    case R_CON:
        if ((value & CON_TCS) && !(s->con & CON_TCS)) {
            qemu_log_mask(LOG_UNIMP,
                          "pic32-timer: external clock source is not "
                          "modelled; counting the peripheral clock\n");
        }
        if (value & CON_T32) {
            qemu_log_mask(LOG_UNIMP,
                          "pic32-timer: 32-bit pairing is not modelled\n");
        }
        if (value & CON_TGATE) {
            qemu_log_mask(LOG_UNIMP,
                          "pic32-timer: gated accumulation is not modelled\n");
        }
        s->con = value;
        pic32_timer_reload(s, false);
        break;
    case R_TMR:
        ptimer_transaction_begin(s->timer);
        ptimer_set_count(s->timer,
                         ((s->pr & 0xFFFF) + 1) - (value & 0xFFFF));
        ptimer_transaction_commit(s->timer);
        break;
    case R_PR:
        s->pr = value & 0xFFFF;
        pic32_timer_reload(s, true);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-timer: write of 0x%08x to 0x%02x\n",
                      value, (unsigned)addr);
        break;
    }
}

static const PIC32RegsOps pic32_timer_regs_ops = {
    .read = pic32_timer_read,
    .write = pic32_timer_write,
};

static void pic32_timer_reset_hold(Object *obj, ResetType type)
{
    PIC32TimerState *s = PIC32_TIMER(obj);

    s->con = 0;
    s->pr = 0xFFFF;
    ptimer_transaction_begin(s->timer);
    ptimer_stop(s->timer);
    ptimer_set_limit(s->timer, TIMER_PERIOD, 1);
    ptimer_transaction_commit(s->timer);
}

static void pic32_timer_realize(DeviceState *dev, Error **errp)
{
    PIC32TimerState *s = PIC32_TIMER(dev);

    if (!clock_has_source(s->pbclk)) {
        error_setg(errp, "pic32-timer: no peripheral clock");
        return;
    }

    s->timer = ptimer_init(pic32_timer_tick, s, PTIMER_POLICY_LEGACY);

    pic32_regs_init_io(&s->mmio, OBJECT(dev), &pic32_timer_regs_ops, s,
                       "pic32-timer", PIC32_TIMER_SIZE, PIC32_REGS_ALIASED);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    qdev_init_gpio_out_named(dev, &s->irq, PIC32_TIMER_IRQ_GPIO, 1);
}

static void pic32_timer_init(Object *obj)
{
    PIC32TimerState *s = PIC32_TIMER(obj);

    s->pbclk = qdev_init_clock_in(DEVICE(obj), "pbclk", NULL, NULL, 0);
}

static const Property pic32_timer_properties[] = {
    DEFINE_PROP_BOOL("type-a", PIC32TimerState, type_a, false),
};

static const VMStateDescription pic32_timer_vmstate = {
    .name = "pic32-timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(con, PIC32TimerState),
        VMSTATE_UINT32(pr, PIC32TimerState),
        VMSTATE_PTIMER(timer, PIC32TimerState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic32_timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic32_timer_realize;
    dc->vmsd = &pic32_timer_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, pic32_timer_properties);
    rc->phases.hold = pic32_timer_reset_hold;
}

static const TypeInfo pic32_timer_types[] = {
    {
        .name = TYPE_PIC32_TIMER,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC32TimerState),
        .instance_init = pic32_timer_init,
        .class_init = pic32_timer_class_init,
    },
};

DEFINE_TYPES(pic32_timer_types)
