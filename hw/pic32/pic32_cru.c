/*
 * PIC32 clock, reset and system configuration
 *
 * Two blocks of register space that belong together: CFG-PMD, which holds the
 * device ID, the unlock key and the peripheral module disable bits, and CRU,
 * which holds the oscillator, PLL, reference clock and peripheral bus divider
 * registers.
 *
 * The clock tree itself is not modelled. This part comes out of reset running
 * from the PLL its configuration words select and the firmware never switches
 * away, so the frequencies are fixed and the SoC publishes them; what these
 * registers have to do is remember what was written, report the "ready" and
 * "locked" bits that firmware polls, and refuse the writes hardware would
 * refuse. Getting that last part wrong is silent: a clock register that
 * accepts a write it should have ignored looks like nothing at all until the
 * guest divides by it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/pic32/pic32_cru.h"
#include "hw/pic32/pic32_regs.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "system/runstate.h"

/* CFG-PMD block. */
enum {
    R_CFGCON  = 0x000,
    R_DEVID   = 0x020,
    R_SYSKEY  = 0x030,
    R_PMD1    = 0x040,
    R_PMD7    = 0x0A0,
    R_CFGPG   = 0x0E0,
    R_CFGAPP2 = 0x100,
    R_CFGCON2 = 0x110,
};

/* CRU block. */
enum {
    R_OSCCON    = 0x000,
    R_OSCTUN    = 0x010,
    R_SPLLCON   = 0x020,
    R_UPLLCON   = 0x030,
    R_RCON      = 0x040,
    R_RSWRST    = 0x050,
    R_RNMICON   = 0x060,
    R_PWRCON    = 0x070,
    R_REFO1CON  = 0x080,
    R_REFO4TRIM = 0x0F0,
    R_PB1DIV    = 0x100,
    R_PB7DIV    = 0x160,
    R_SLEWCON   = 0x180,
    R_CLKSTAT   = 0x190,
};

/* The two halves of the unlock sequence, in order. */
#define SYSKEY_UNLOCK1 0xAA996655u
#define SYSKEY_UNLOCK2 0x556699AAu

/* OSCCON */
#define OSCCON_OSWEN  (1u << 0)
#define OSCCON_SLPEN  (1u << 4)
#define OSCCON_CF     (1u << 3)
#define OSCCON_SLOCK  (1u << 7)
#define OSCCON_ULOCK  (1u << 6)
#define OSCCON_NOSC   (7u << 8)
#define OSCCON_COSC   (7u << 12)
#define OSCCON_NOSC_SHIFT 8
#define OSCCON_COSC_SHIFT 12

/* PBxDIV */
#define PBDIV_PBDIV    0x7Fu
#define PBDIV_PBDIVRDY (1u << 11)
#define PBDIV_ON       (1u << 15)



/* RSWRST */
#define RSWRST_SWRST (1u << 0)

bool pic32_cru_unlocked(PIC32CruState *s)
{
    return s->unlock_step == 2;
}

void pic32_cru_set_reset_cause(PIC32CruState *s, uint32_t rcon_bits)
{
    s->rcon_pending = rcon_bits;
}

/*
 * SYSKEY guards the clock registers and a handful of others. Two magic values
 * in order unlock; anything else at any point locks again, which is why
 * CLK_Initialize() writes a zero first -- it is making sure a half-finished
 * sequence cannot combine with its own.
 */
static void pic32_cru_syskey_write(PIC32CruState *s, uint32_t value)
{
    if (s->unlock_step == 0 && value == SYSKEY_UNLOCK1) {
        s->unlock_step = 1;
    } else if (s->unlock_step == 1 && value == SYSKEY_UNLOCK2) {
        s->unlock_step = 2;
    } else {
        s->unlock_step = 0;
    }
    s->syskey = value;
}

static uint32_t pic32_cru_cfg_read(void *opaque, hwaddr addr)
{
    PIC32CruState *s = opaque;

    switch (addr) {
    case R_CFGCON:
        return s->cfgcon;
    case R_DEVID:
        return s->devid;
    case R_SYSKEY:
        return s->syskey;
    case R_PMD1 ... R_PMD7:
        return s->pmd[(addr - R_PMD1) / 0x10];
    case R_CFGPG:
        return s->cfgpg;
    case R_CFGAPP2:
        return s->cfgapp2;
    case R_CFGCON2:
        return s->cfgcon2;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-cru: read of cfg + 0x%03x\n",
                      (unsigned)addr);
        return 0;
    }
}

static void pic32_cru_cfg_write(void *opaque, hwaddr addr, uint32_t value)
{
    PIC32CruState *s = opaque;

    switch (addr) {
    case R_CFGCON:
        s->cfgcon = value;
        break;
    case R_DEVID:
        break;
    case R_SYSKEY:
        pic32_cru_syskey_write(s, value);
        break;
    case R_PMD1 ... R_PMD7:
        /*
         * Stored and otherwise ignored. Disabling a module on hardware stops
         * its clock and makes its registers read as zero; every module this
         * machine models is one the firmware leaves enabled, so honouring the
         * bits would only add a way to lose accesses silently.
         */
        s->pmd[(addr - R_PMD1) / 0x10] = value;
        break;
    case R_CFGPG:
        s->cfgpg = value;
        break;
    case R_CFGAPP2:
        s->cfgapp2 = value;
        break;
    case R_CFGCON2:
        s->cfgcon2 = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-cru: write of 0x%08x to cfg + 0x%03x\n",
                      value, (unsigned)addr);
        break;
    }
}

static uint32_t pic32_cru_osc_read(void *opaque, hwaddr addr)
{
    PIC32CruState *s = opaque;

    switch (addr) {
    case R_OSCCON:
        return s->osccon;
    case R_OSCTUN:
        return s->osctun;
    case R_SPLLCON:
        return s->spllcon;
    case R_UPLLCON:
        return s->upllcon;
    case R_RCON:
        return s->rcon;
    case R_RSWRST:
        return 0;
    case R_RNMICON:
        return s->rnmicon;
    case R_PWRCON:
        return s->pwrcon;
    case R_REFO1CON ... R_REFO4TRIM:
        if ((addr - R_REFO1CON) & 0x10) {
            return s->refotrim[(addr - R_REFO1CON) / 0x20];
        }
        return s->refocon[(addr - R_REFO1CON) / 0x20];
    case R_PB1DIV ... R_PB7DIV:
        return s->pbdiv[(addr - R_PB1DIV) / 0x10];
    case R_SLEWCON:
        return s->slewcon;
    case R_CLKSTAT:
        /*
         * Every oscillator reports itself ready. Nothing here models one
         * warming up, and a zero would hang any firmware that waits.
         */
        return 0xFF;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-cru: read of osc + 0x%03x\n",
                      (unsigned)addr);
        return 0;
    }
}

static void pic32_cru_osc_write(void *opaque, hwaddr addr, uint32_t value)
{
    PIC32CruState *s = opaque;
    unsigned i;

    /*
     * Writes to this block need the unlock. Hardware simply drops one made
     * without it, which is a silent way to end up at the wrong clock, so say
     * so rather than only doing the dropping.
     */
    if (!pic32_cru_unlocked(s) && addr != R_RCON) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pic32-cru: write of 0x%08x to osc + 0x%03x while "
                      "locked; ignored as hardware would\n",
                      value, (unsigned)addr);
        return;
    }

    switch (addr) {
    case R_OSCCON:
        /*
         * A clock switch completes at once: the new source becomes the current
         * one and OSWEN clears itself, which is what firmware polls for. The
         * lock bits stay set because both PLLs are always locked here.
         */
        s->osccon = (s->osccon & ~(OSCCON_NOSC | OSCCON_SLPEN | OSCCON_CF)) |
                    (value & (OSCCON_NOSC | OSCCON_SLPEN));
        if (value & OSCCON_OSWEN) {
            i = (value & OSCCON_NOSC) >> OSCCON_NOSC_SHIFT;
            s->osccon = (s->osccon & ~OSCCON_COSC) | (i << OSCCON_COSC_SHIFT);
        }
        break;
    case R_OSCTUN:
        s->osctun = value;
        break;
    case R_SPLLCON:
        s->spllcon = value;
        break;
    case R_UPLLCON:
        s->upllcon = value;
        break;
    case R_RCON:
        s->rcon = value;
        break;
    case R_RSWRST:
        /*
         * The reset takes effect when the register is read back, not when it
         * is written -- DS60001118 7.2. Nothing here waits for the read: the
         * write is the only thing a guest does with it, and a reset that
         * needed a read to arrive would just be a way to miss one.
         */
        if (value & RSWRST_SWRST) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        break;
    case R_RNMICON:
        s->rnmicon = value;
        break;
    case R_PWRCON:
        s->pwrcon = value;
        break;
    case R_REFO1CON ... R_REFO4TRIM:
        if ((addr - R_REFO1CON) & 0x10) {
            s->refotrim[(addr - R_REFO1CON) / 0x20] = value;
        } else {
            s->refocon[(addr - R_REFO1CON) / 0x20] = value;
        }
        break;
    case R_PB1DIV ... R_PB7DIV:
        /* The divider is always ready by the time the guest can look. */
        s->pbdiv[(addr - R_PB1DIV) / 0x10] =
            (value & (PBDIV_PBDIV | PBDIV_ON)) | PBDIV_PBDIVRDY;
        break;
    case R_SLEWCON:
        s->slewcon = value;
        break;
    case R_CLKSTAT:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-cru: write of 0x%08x to osc + 0x%03x\n",
                      value, (unsigned)addr);
        break;
    }
}

static const PIC32RegsOps pic32_cru_cfg_ops = {
    .read = pic32_cru_cfg_read,
    .write = pic32_cru_cfg_write,
};

static const PIC32RegsOps pic32_cru_osc_ops = {
    .read = pic32_cru_osc_read,
    .write = pic32_cru_osc_write,
};

static void pic32_cru_reset_hold(Object *obj, ResetType type)
{
    PIC32CruState *s = PIC32_CRU(obj);
    unsigned i;

    s->cfgcon = 0;
    s->cfgcon2 = 0;
    s->cfgpg = 0;
    s->cfgapp2 = 0;
    s->syskey = 0;
    s->unlock_step = 0;
    memset(s->pmd, 0, sizeof(s->pmd));

    /*
     * Both PLLs report locked and the current source is the one the
     * configuration words chose, which for this firmware is the system PLL.
     * The SoC publishes the frequency that implies; this register only has to
     * agree with it.
     */
    s->osccon = OSCCON_SLOCK | OSCCON_ULOCK | (1u << OSCCON_COSC_SHIFT) |
                (1u << OSCCON_NOSC_SHIFT);
    s->osctun = 0;
    s->spllcon = 0;
    s->upllcon = 0;
    /*
     * A power-on reset is the default; anything else has to have said so
     * before it happened, since getting here is the last thing that does.
     */
    s->rcon = s->rcon_pending ?: (PIC32_RCON_POR | PIC32_RCON_BOR);
    s->rcon_pending = 0;
    s->rnmicon = 0;
    s->pwrcon = 0;
    memset(s->refocon, 0, sizeof(s->refocon));
    memset(s->refotrim, 0, sizeof(s->refotrim));
    for (i = 0; i < PIC32_CRU_PBDIVS; i++) {
        s->pbdiv[i] = PBDIV_ON | PBDIV_PBDIVRDY | 1;
    }
    s->slewcon = 0;
}

static void pic32_cru_realize(DeviceState *dev, Error **errp)
{
    PIC32CruState *s = PIC32_CRU(dev);

    pic32_regs_init_io(&s->cfg, OBJECT(dev), &pic32_cru_cfg_ops, s,
                       "pic32-cru-cfg", PIC32_CRU_CFG_SIZE,
                       PIC32_REGS_ALIASED);
    pic32_regs_init_io(&s->osc, OBJECT(dev), &pic32_cru_osc_ops, s,
                       "pic32-cru-osc", PIC32_CRU_OSC_SIZE,
                       PIC32_REGS_ALIASED);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->cfg);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->osc);
}

static const Property pic32_cru_properties[] = {
    DEFINE_PROP_UINT32("devid", PIC32CruState, devid, 0),
};

static const VMStateDescription pic32_cru_vmstate = {
    .name = "pic32-cru",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cfgcon, PIC32CruState),
        VMSTATE_UINT32(cfgcon2, PIC32CruState),
        VMSTATE_UINT32(cfgpg, PIC32CruState),
        VMSTATE_UINT32(cfgapp2, PIC32CruState),
        VMSTATE_UINT32(syskey, PIC32CruState),
        VMSTATE_UINT32_ARRAY(pmd, PIC32CruState, PIC32_CRU_PMDS),
        VMSTATE_UINT32(osccon, PIC32CruState),
        VMSTATE_UINT32(osctun, PIC32CruState),
        VMSTATE_UINT32(spllcon, PIC32CruState),
        VMSTATE_UINT32(upllcon, PIC32CruState),
        VMSTATE_UINT32(rcon, PIC32CruState),
        VMSTATE_UINT32(rnmicon, PIC32CruState),
        VMSTATE_UINT32(pwrcon, PIC32CruState),
        VMSTATE_UINT32_ARRAY(refocon, PIC32CruState, PIC32_CRU_REFOS),
        VMSTATE_UINT32_ARRAY(refotrim, PIC32CruState, PIC32_CRU_REFOS),
        VMSTATE_UINT32_ARRAY(pbdiv, PIC32CruState, PIC32_CRU_PBDIVS),
        VMSTATE_UINT32(slewcon, PIC32CruState),
        VMSTATE_UINT32(unlock_step, PIC32CruState),
        VMSTATE_UINT32(rcon_pending, PIC32CruState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic32_cru_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic32_cru_realize;
    dc->vmsd = &pic32_cru_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, pic32_cru_properties);
    rc->phases.hold = pic32_cru_reset_hold;
}

static const TypeInfo pic32_cru_types[] = {
    {
        .name = TYPE_PIC32_CRU,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC32CruState),
        .class_init = pic32_cru_class_init,
    },
};

DEFINE_TYPES(pic32_cru_types)
