/*
 * PIC16 Windowed Watchdog Timer
 *
 * Runs from the low-frequency internal oscillator, which is independent of the
 * system clock, and resets the device if it is not cleared within its period.
 * The window -- the part of the period during which a CLRWDT is *too early*
 * and itself a fault -- is not modelled; the reference firmware leaves it
 * fully open.
 *
 * The timer is on QEMU_CLOCK_VIRTUAL so that under -icount the period stays in
 * proportion to instruction execution. Without that a watchdog is either
 * useless or fires constantly, depending on how fast the host is.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "pic16_wwdt.h"

enum {
    REG_WDTCON0,
    REG_WDTCON1,
    REG_WDTPSL,
    REG_WDTPSH,
    REG_WDTTMR,
    PIC16_WWDT_NREGS,
};

#define WDTCON0_SEN   0     /* software enable, when the config bits defer */
#define WDTCON0_PS_SHIFT 1  /* period select, 1:32 << PS of the LFINTOSC */
#define WDTCON0_PS_MASK  0x1F

/* The watchdog runs from the 31 kHz low-frequency oscillator. */
#define LFINTOSC_HZ 31000

/*
 * WDTCPS in the configuration words picks the period as a divisor of the
 * LFINTOSC. The reference firmware uses WDTCPS_11, which is 1:65536 -- about
 * 2.1 seconds. Without configuration-word decoding the same default applies,
 * which is the common case.
 */
#define WDTCPS_DEFAULT_DIV 65536

static uint32_t pic16_wwdt_divisor(PIC16WwdtState *s)
{
    unsigned ps = (s->con0 >> WDTCON0_PS_SHIFT) & WDTCON0_PS_MASK;

    if (ps == 0) {
        return WDTCPS_DEFAULT_DIV;
    }
    /* PS selects 1:32 doubling per step, saturating at the 1:8388608 setting. */
    return ps > 18 ? (1u << 23) : (32u << (ps - 1));
}

static void pic16_wwdt_rearm(PIC16WwdtState *s)
{
    uint32_t div = pic16_wwdt_divisor(s);

    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, LFINTOSC_HZ);
    ptimer_set_limit(s->timer, div, 1);
    if (s->con0 & (1u << WDTCON0_SEN)) {
        ptimer_run(s->timer, 1);
    } else {
        ptimer_stop(s->timer);
    }
    ptimer_transaction_commit(s->timer);
}

static void pic16_wwdt_expire(void *opaque)
{
    PIC16WwdtState *s = opaque;

    s->expired = true;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "pic16-wwdt: watchdog expired, resetting\n");
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

/* The CPU pulses this line when it executes CLRWDT. */
static void pic16_wwdt_clear(void *opaque, int line, int level)
{
    PIC16WwdtState *s = opaque;

    if (level) {
        pic16_wwdt_rearm(s);
    }
}

static uint64_t pic16_wwdt_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC16WwdtState *s = opaque;

    switch (addr) {
    case REG_WDTCON0:
        return s->con0;
    case REG_WDTCON1:
        return s->con1;
    case REG_WDTTMR:
        /* Fraction of the period elapsed, in the top five bits. */
        return ((pic16_wwdt_divisor(s) - ptimer_get_count(s->timer)) * 32 /
                pic16_wwdt_divisor(s)) & 0x1F;
    default:
        return 0;   /* the prescale counters are not modelled */
    }
}

static void pic16_wwdt_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    PIC16WwdtState *s = opaque;

    switch (addr) {
    case REG_WDTCON0:
        s->con0 = value;
        pic16_wwdt_rearm(s);
        break;
    case REG_WDTCON1:
        s->con1 = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pic16_wwdt_ops = {
    .read = pic16_wwdt_read,
    .write = pic16_wwdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic16_wwdt_reset_hold(Object *obj, ResetType type)
{
    PIC16WwdtState *s = PIC16_WWDT(obj);

    s->con1 = 0x07;

    uint32_t wdte = PIC16_CONFIG3_WDTE_ON;
    if (s->cpu) {
        wdte = pic16_wdte(&s->cpu->env);
    }

    if (wdte == PIC16_CONFIG3_WDTE_ON) {
        s->con0 = 1u << WDTCON0_SEN;
    } else {
        s->con0 = 0;
    }
    pic16_wwdt_rearm(s);
}

static void pic16_wwdt_realize(DeviceState *dev, Error **errp)
{
    PIC16WwdtState *s = PIC16_WWDT(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->timer = ptimer_init(pic16_wwdt_expire, s, PTIMER_POLICY_LEGACY);

    memory_region_init_io(&s->iomem, OBJECT(dev), &pic16_wwdt_ops, s,
                          "pic16.wwdt", PIC16_WWDT_NREGS);
    sysbus_init_mmio(sbd, &s->iomem);

    qdev_init_gpio_in_named(dev, pic16_wwdt_clear, PIC16_WWDT_CLEAR_GPIO, 1);
}

static const VMStateDescription pic16_wwdt_vmstate = {
    .name = "pic16-wwdt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(timer, PIC16WwdtState),
        VMSTATE_UINT8(con0, PIC16WwdtState),
        VMSTATE_UINT8(con1, PIC16WwdtState),
        VMSTATE_BOOL(expired, PIC16WwdtState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic16_wwdt_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic16_wwdt_realize;
    dc->vmsd = &pic16_wwdt_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = pic16_wwdt_reset_hold;
}

static const TypeInfo pic16_wwdt_types[] = {
    {
        .name = TYPE_PIC16_WWDT,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC16WwdtState),
        .class_init = pic16_wwdt_class_init,
    },
};

DEFINE_TYPES(pic16_wwdt_types)
