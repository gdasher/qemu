/*
 * PIC18F-Q43 Windowed Watchdog Timer
 *
 * WDTCON0/WDTCON1/WDTPSL/WDTPSH/WDTTMR at 0x78. WDTE (CONFIG5<6:5>) and
 * WDTCPS (CONFIG5<4:0>) come from the CPU's configuration bytes.
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
#include "pic18_wwdt.h"

enum {
    REG_WDTCON0,
    REG_WDTCON1,
    REG_WDTPSL,
    REG_WDTPSH,
    REG_WDTTMR,
    PIC18_WWDT_NREGS,
};

#define WDTCON0_SEN   0     /* software enable, when the config bits defer */
#define WDTCON0_PS_SHIFT 1  /* period select, 1:32 << PS of the LFINTOSC */
#define WDTCON0_PS_MASK  0x1F

/* WDTPS values above this are reserved and give the minimum period. */
#define WDTPS_MAX 18
#define WDTPS_SW_RESET 0x0B /* 1:65536 when WDTCPS leaves it to software */

/* The watchdog runs from the 31 kHz low-frequency oscillator. */
#define LFINTOSC_HZ 31000

/* CONFIG5 encodings (PIC18F27Q43 data sheet, Configuration Word 5). */
#define PIC18_CONFIG5_WDTE_OFF    0   /* watchdog off */
#define PIC18_CONFIG5_WDTE_SWDTEN 1   /* on when WDTCON0.SEN is set */
#define PIC18_CONFIG5_WDTE_NSLEEP 2   /* on while not sleeping */
#define PIC18_CONFIG5_WDTE_ON     3   /* always on */
#define PIC18_CONFIG5_WDTCPS_SW   0x1F /* period from WDTCON0.WDTPS */

static uint32_t pic18_wwdt_divisor(PIC18WwdtState *s)
{
    unsigned ps = (s->con0 >> WDTCON0_PS_SHIFT) & WDTCON0_PS_MASK;

    return ps > WDTPS_MAX ? 32u : (32u << ps);
}

/*
 * WDTE in the configuration words decides whether SWDTEN has a say. With no
 * CPU to ask (the unit is unconnected) it behaves as if always enabled, the
 * shipped default of an erased part.
 */
static bool pic18_wwdt_enabled(PIC18WwdtState *s)
{
    uint32_t wdte = s->cpu ? pic18_wdte(&s->cpu->env) : PIC18_CONFIG5_WDTE_ON;

    switch (wdte) {
    case PIC18_CONFIG5_WDTE_OFF:
        return false;
    case PIC18_CONFIG5_WDTE_SWDTEN:
        return (s->con0 & (1u << WDTCON0_SEN)) != 0;
    default:
        /* Always on, or on except in Sleep, which is not distinguished. */
        return true;
    }
}

static bool pic18_wwdt_ps_writable(PIC18WwdtState *s)
{
    return !s->cpu || pic18_wdtcps(&s->cpu->env) == PIC18_CONFIG5_WDTCPS_SW;
}

static void pic18_wwdt_rearm(PIC18WwdtState *s)
{
    uint32_t div = pic18_wwdt_divisor(s);

    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, LFINTOSC_HZ);
    ptimer_set_limit(s->timer, div, 1);
    if (pic18_wwdt_enabled(s)) {
        ptimer_run(s->timer, 1);
    } else {
        ptimer_stop(s->timer);
    }
    ptimer_transaction_commit(s->timer);
}

static void pic18_wwdt_expire(void *opaque)
{
    PIC18WwdtState *s = opaque;

    s->expired = true;
    if (s->cpu) {
        /* So PCON0.nRWDT can say why the device reset. */
        s->cpu->env.reset_wdt = 1;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "pic18-wwdt: watchdog expired, resetting\n");
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

/* The CPU pulses this line when it executes CLRWDT. */
static void pic18_wwdt_clear(void *opaque, int line, int level)
{
    PIC18WwdtState *s = opaque;

    if (level) {
        pic18_wwdt_rearm(s);
    }
}

static uint64_t pic18_wwdt_read(void *opaque, hwaddr addr, unsigned size)
{
    PIC18WwdtState *s = opaque;

    switch (addr) {
    case REG_WDTCON0:
        return s->con0;
    case REG_WDTCON1:
        return s->con1;
    case REG_WDTTMR:
        /* Fraction of the period elapsed, in the top five bits. */
        return ((pic18_wwdt_divisor(s) - ptimer_get_count(s->timer)) * 32 /
                pic18_wwdt_divisor(s)) & 0x1F;
    default:
        return 0;   /* the prescale counters are not modelled */
    }
}

static void pic18_wwdt_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    PIC18WwdtState *s = opaque;

    switch (addr) {
    case REG_WDTCON0:
        /* WDTPS is only writable when WDTCPS leaves the period to software. */
        if (!pic18_wwdt_ps_writable(s)) {
            value = (value & ~(WDTCON0_PS_MASK << WDTCON0_PS_SHIFT)) |
                    (s->con0 & (WDTCON0_PS_MASK << WDTCON0_PS_SHIFT));
        }
        s->con0 = value & ((WDTCON0_PS_MASK << WDTCON0_PS_SHIFT) |
                           (1u << WDTCON0_SEN));
        pic18_wwdt_rearm(s);
        break;
    case REG_WDTCON1:
        s->con1 = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pic18_wwdt_ops = {
    .read = pic18_wwdt_read,
    .write = pic18_wwdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pic18_wwdt_reset_hold(Object *obj, ResetType type)
{
    PIC18WwdtState *s = PIC18_WWDT(obj);

    uint32_t wdtcps = s->cpu ? pic18_wdtcps(&s->cpu->env)
                             : PIC18_CONFIG5_WDTCPS_SW;

    s->con1 = 0x07;

    /* WDTPS resets to the configured period; SWDTEN to off. */
    if (wdtcps == PIC18_CONFIG5_WDTCPS_SW) {
        wdtcps = WDTPS_SW_RESET;
    }
    s->con0 = wdtcps << WDTCON0_PS_SHIFT;
    pic18_wwdt_rearm(s);
}

static void pic18_wwdt_realize(DeviceState *dev, Error **errp)
{
    PIC18WwdtState *s = PIC18_WWDT(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->timer = ptimer_init(pic18_wwdt_expire, s, PTIMER_POLICY_LEGACY);

    memory_region_init_io(&s->iomem, OBJECT(dev), &pic18_wwdt_ops, s,
                          "pic18.wwdt", PIC18_WWDT_NREGS);
    sysbus_init_mmio(sbd, &s->iomem);

    qdev_init_gpio_in_named(dev, pic18_wwdt_clear, PIC18_WWDT_CLEAR_GPIO, 1);
}

static const VMStateDescription pic18_wwdt_vmstate = {
    .name = "pic18-wwdt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(timer, PIC18WwdtState),
        VMSTATE_UINT8(con0, PIC18WwdtState),
        VMSTATE_UINT8(con1, PIC18WwdtState),
        VMSTATE_BOOL(expired, PIC18WwdtState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic18_wwdt_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic18_wwdt_realize;
    dc->vmsd = &pic18_wwdt_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = pic18_wwdt_reset_hold;
}

static const TypeInfo pic18_wwdt_types[] = {
    {
        .name = TYPE_PIC18_WWDT,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC18WwdtState),
        .class_init = pic18_wwdt_class_init,
    },
};

DEFINE_TYPES(pic18_wwdt_types)
