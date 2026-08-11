/*
 * PIC32 watchdog timer
 *
 * A counter off the low-power oscillator that resets the part unless software
 * feeds it, which it does by storing a key into the top half of WDTCON:
 *
 *     *(volatile uint16_t *)&WDTCON + 1 = 0x5743;
 *
 * That halfword store is the reason the register fabric merges narrow accesses
 * rather than widening them -- a widened one would arrive with the key in the
 * bottom half and the watchdog would never see a feed.
 *
 * This firmware means it: the comment on the one call site says any state that
 * stops emitting frames should let the watchdog reset the board. So the model
 * is faithful and the machine leaves it disabled unless asked, because an
 * armed watchdog resets the board during every bring-up step that stops short
 * of frames, and hides whatever was actually being looked at.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/pic32/pic32_regs.h"
#include "hw/pic32/pic32_wdt.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "system/runstate.h"

#define R_CON 0x00

/* WDTCON */
#define CON_WDTWINEN  (1u << 0)
#define CON_SLPDIV    (0x1Fu << 1)
#define CON_SLPDIV_SHIFT 1
#define CON_RUNDIV    (0x1Fu << 8)
#define CON_RUNDIV_SHIFT 8
#define CON_ON        (1u << 15)
#define CON_KEY_SHIFT 16
#define CON_KEY       0xFFFF0000u

/* The key that feeds it, in the top half. */
#define WDT_CLRKEY 0x5743

/* The watchdog counts the low-power oscillator, which runs at 32.768 kHz. */
#define WDT_CLOCK_HZ 32768

static uint32_t pic32_wdt_period(PIC32WdtState *s)
{
    unsigned rundiv = (s->con & CON_RUNDIV) >> CON_RUNDIV_SHIFT;

    /* The divider is a power of two, and the field is which power. */
    return 1u << MIN(rundiv, 20);
}

static void pic32_wdt_reload(PIC32WdtState *s)
{
    ptimer_transaction_begin(s->timer);
    ptimer_set_limit(s->timer, pic32_wdt_period(s), 1);
    if (s->con & CON_ON) {
        ptimer_run(s->timer, 1);
    } else {
        ptimer_stop(s->timer);
    }
    ptimer_transaction_commit(s->timer);
}

static void pic32_wdt_expire(void *opaque)
{
    PIC32WdtState *s = opaque;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "pic32-wdt: not fed for %u ticks; resetting\n",
                  pic32_wdt_period(s));
    qemu_set_irq(s->timeout, 1);
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static uint32_t pic32_wdt_read(void *opaque, hwaddr addr)
{
    PIC32WdtState *s = opaque;

    switch (addr) {
    case R_CON:
        /* The key is write-only and reads back as zero. */
        return s->con & ~CON_KEY;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-wdt: read of 0x%02x\n",
                      (unsigned)addr);
        return 0;
    }
}

static void pic32_wdt_write(void *opaque, hwaddr addr, uint32_t value)
{
    PIC32WdtState *s = opaque;
    bool was_on;

    switch (addr) {
    case R_CON:
        if (((value & CON_KEY) >> CON_KEY_SHIFT) == WDT_CLRKEY) {
            /*
             * A feed. It carries the rest of the register with it, so only the
             * count is reset here -- writing the key is not a way to turn the
             * watchdog on or off.
             */
            pic32_wdt_reload(s);
            return;
        }
        was_on = s->con & CON_ON;
        /*
         * Only ON and WDTWINEN belong to software. RUNDIV and SLPDIV are
         * read-only mirrors of the WDTPS configuration bits, whatever was
         * written over them.
         */
        s->con = value & (CON_ON | CON_WDTWINEN);
        s->con |= (s->rundiv << CON_RUNDIV_SHIFT) & CON_RUNDIV;
        s->con |= (s->rundiv << CON_SLPDIV_SHIFT) & CON_SLPDIV;
        if (s->enabled && !(s->con & CON_ON)) {
            /*
             * With FWDTEN set the watchdog cannot be disabled by software:
             * the ON bit stays set no matter what is written to it.
             */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pic32-wdt: FWDTEN is set; ignoring an attempt to "
                          "disable the watchdog\n");
            s->con |= CON_ON;
        }
        if (!was_on != !(s->con & CON_ON) || !was_on) {
            pic32_wdt_reload(s);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-wdt: write of 0x%08x to 0x%02x\n",
                      value, (unsigned)addr);
        break;
    }
}

static const PIC32RegsOps pic32_wdt_regs_ops = {
    .read = pic32_wdt_read,
    .write = pic32_wdt_write,
};

static void pic32_wdt_reset_hold(Object *obj, ResetType type)
{
    PIC32WdtState *s = PIC32_WDT(obj);

    /*
     * The configuration words decide whether the watchdog is already running
     * when the first instruction executes, and what it counts to.
     */
    s->con = (s->rundiv << CON_RUNDIV_SHIFT) & CON_RUNDIV;
    s->con |= (s->rundiv << CON_SLPDIV_SHIFT) & CON_SLPDIV;
    if (s->enabled) {
        s->con |= CON_ON;
    }
    qemu_set_irq(s->timeout, 0);
    pic32_wdt_reload(s);
}

static void pic32_wdt_realize(DeviceState *dev, Error **errp)
{
    PIC32WdtState *s = PIC32_WDT(dev);

    s->timer = ptimer_init(pic32_wdt_expire, s, PTIMER_POLICY_LEGACY);
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, WDT_CLOCK_HZ);
    ptimer_transaction_commit(s->timer);

    pic32_regs_init_io(&s->mmio, OBJECT(dev), &pic32_wdt_regs_ops, s,
                       "pic32-wdt", PIC32_WDT_SIZE, PIC32_REGS_ALIASED);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    qdev_init_gpio_out_named(dev, &s->timeout, PIC32_WDT_TIMEOUT_GPIO, 1);
}

static const Property pic32_wdt_properties[] = {
    DEFINE_PROP_BOOL("enabled", PIC32WdtState, enabled, false),
    /*
     * 2^13 ticks of 32.768 kHz is 250 ms, which is what this firmware's
     * WDTPS = PS8192 configuration word asks for.
     */
    DEFINE_PROP_UINT32("rundiv", PIC32WdtState, rundiv, 13),
};

static const VMStateDescription pic32_wdt_vmstate = {
    .name = "pic32-wdt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(con, PIC32WdtState),
        VMSTATE_PTIMER(timer, PIC32WdtState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic32_wdt_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic32_wdt_realize;
    dc->vmsd = &pic32_wdt_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, pic32_wdt_properties);
    rc->phases.hold = pic32_wdt_reset_hold;
}

static const TypeInfo pic32_wdt_types[] = {
    {
        .name = TYPE_PIC32_WDT,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC32WdtState),
        .class_init = pic32_wdt_class_init,
    },
};

DEFINE_TYPES(pic32_wdt_types)
