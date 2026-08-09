/*
 * PIC32 interrupt controller
 *
 * The EVIC resolves 256 sources down to one request and hands the core a
 * priority and the address of the handler, taken from that source's OFFx
 * register. That last part is why the core needed a change: the architectural
 * vectored-interrupt mode computes the handler address from the priority and a
 * fixed spacing, which cannot address 256 separately-placed handlers, and it
 * is not how any PIC32 works. The core now takes the offset from the
 * controller when there is one.
 *
 * The two core software interrupts are ordinary sources here, and their flags
 * live in the core rather than in this device: writing Cause.IP0 raises one,
 * and clearing IFS0's flag has to reach back and clear that bit again. The
 * FreeRTOS port yields exactly that way, so nothing runs without it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/pic32/pic32_evic.h"
#include "hw/pic32/pic32_dmac.h"
#include "hw/pic32/pic32_regs.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

enum {
    R_INTCON = 0x000,
    R_PRISS  = 0x010,
    R_INTSTAT = 0x020,
    R_IPTMR  = 0x030,
    R_IFS0   = 0x040,
    R_IEC0   = 0x0C0,
    R_IPC0   = 0x140,
};

/* INTCON */
#define INTCON_MVEC (1u << 12)

/* INTSTAT */
#define INTSTAT_SIRQ_MASK 0xFF
#define INTSTAT_SRIPL_SHIFT 8

/*
 * Each IPCx register holds four sources, a byte each: priority in bits 4:2 of
 * the byte, subpriority in bits 1:0.
 */
#define IPC_PRIORITY(reg, src) (((reg) >> (((src) & 3) * 8 + 2)) & 0x7)
#define IPC_SUBPRIORITY(reg, src) (((reg) >> (((src) & 3) * 8)) & 0x3)

/* OFFx carries the offset in bits 17:1; bit 0 reads as zero. */
#define OFF_MASK 0x0003FFFEu

/* The sources in one flag register that are both flagged and enabled. */
static uint32_t pic32_evic_pending(PIC32EvicState *s, unsigned reg)
{
    return (s->ifs[reg] | s->level[reg]) & s->iec[reg];
}

/*
 * Picks the source the core should be told about: highest priority, then
 * highest subpriority, then -- as the data sheet's "natural order" -- lowest
 * source number. Priority 0 means the source is disabled however its enable
 * bit reads, so it can never win.
 */
static void pic32_evic_update(PIC32EvicState *s)
{
    unsigned best = 0, best_pri = 0, best_sub = 0;
    bool found = false;
    unsigned reg;
    uint32_t ripl, offset;

    /*
     * Walked a set bit at a time rather than a source at a time: a peripheral
     * that interrupts on every word it moves -- the parallel port does, and
     * this firmware pushes ten thousand words through it a frame -- would
     * otherwise pay for all 256 sources on each one.
     */
    for (reg = 0; reg < PIC32_EVIC_IFS_REGS; reg++) {
        uint32_t bits = pic32_evic_pending(s, reg);

        while (bits) {
            unsigned bit = ctz32(bits);
            unsigned src = reg * 32 + bit;
            unsigned pri = IPC_PRIORITY(s->ipc[src / 4], src);
            unsigned sub = IPC_SUBPRIORITY(s->ipc[src / 4], src);

            bits &= bits - 1;
            if (pri == 0) {
                continue;
            }
            if (!found || pri > best_pri ||
                (pri == best_pri && sub > best_sub)) {
                found = true;
                best = src;
                best_pri = pri;
                best_sub = sub;
            }
        }
    }

    ripl = found ? best_pri : 0;
    /*
     * In single-vector mode every source arrives at the same handler, the one
     * at offset 0. Firmware here sets MVEC, but a machine that quietly
     * vectored anyway would hide the difference.
     */
    if (!found) {
        offset = 0;
    } else if (s->intcon & INTCON_MVEC) {
        offset = s->offset[best];
    } else {
        offset = 0;
    }

    if (ripl == s->requested_ripl && offset == s->requested_offset) {
        return;
    }
    s->requested_ripl = ripl;
    s->requested_offset = offset;
    s->requested_src = found ? best : 0;
    cpu_mips_eic_request(s->cpu, ripl, offset);
}

/*
 * A latching source. The peripheral pulses the line and the flag stays set
 * until software clears it.
 */
static void pic32_evic_set_irq(void *opaque, int src, int level)
{
    PIC32EvicState *s = opaque;

    /*
     * The DMA controller watches the same sources, and watches them before
     * this does anything with IEC: a channel armed on a source runs whether
     * or not anyone has enabled the interrupt for it.
     */
    if (s->dmac) {
        pic32_dmac_irq_event(s->dmac, src, level);
    }

    if (level) {
        s->ifs[src / 32] |= 1u << (src % 32);
        pic32_evic_update(s);
    }
}

/* A held source. The flag reads as set for as long as the line is driven. */
static void pic32_evic_set_irq_level(void *opaque, int src, int level)
{
    PIC32EvicState *s = opaque;
    uint32_t bit = 1u << (src % 32);

    if (s->dmac) {
        pic32_dmac_irq_event(s->dmac, src, level);
    }

    if (level) {
        s->level[src / 32] |= bit;
    } else {
        s->level[src / 32] &= ~bit;
    }
    pic32_evic_update(s);
}

/*
 * The core software interrupts are raised by the guest writing Cause, and the
 * core drives them in here as held lines. Clearing the flag in IFS0 therefore
 * has to clear the bit in Cause, or the line simply re-asserts and the handler
 * runs for ever.
 */
static void pic32_evic_clear_core_sw(PIC32EvicState *s, uint32_t cleared)
{
    CPUMIPSState *env = &s->cpu->env;
    unsigned i;

    for (i = 0; i < 2; i++) {
        unsigned src = PIC32_IRQ_CORE_SW0 + i;

        if (cleared & (1u << src)) {
            env->CP0_Cause &= ~(1u << (CP0Ca_IP + i));
            s->level[0] &= ~(1u << src);
        }
    }
}

static uint32_t pic32_evic_ctrl_read(void *opaque, hwaddr addr)
{
    PIC32EvicState *s = opaque;
    unsigned i;

    switch (addr) {
    case R_INTCON:
        return s->intcon;
    case R_PRISS:
        return s->priss;
    case R_INTSTAT:
        /* The source the controller is presenting, and at what priority. */
        return (s->requested_ripl << INTSTAT_SRIPL_SHIFT) |
               (s->requested_src & INTSTAT_SIRQ_MASK);
    case R_IPTMR:
        return s->iptmr;
    case R_IFS0 ... R_IFS0 + 0x7F:
        i = (addr - R_IFS0) / 0x10;
        return s->ifs[i] | s->level[i];
    case R_IEC0 ... R_IEC0 + 0x7F:
        return s->iec[(addr - R_IEC0) / 0x10];
    case R_IPC0 ... R_IPC0 + 0x3FF:
        return s->ipc[(addr - R_IPC0) / 0x10];
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-evic: read of 0x%03x\n",
                      (unsigned)addr);
        return 0;
    }
}

static void pic32_evic_ctrl_write(void *opaque, hwaddr addr, uint32_t value)
{
    PIC32EvicState *s = opaque;
    unsigned i;

    switch (addr) {
    case R_INTCON:
        s->intcon = value;
        pic32_evic_update(s);
        break;
    case R_PRISS:
        s->priss = value;
        break;
    case R_INTSTAT:
        break;
    case R_IPTMR:
        s->iptmr = value;
        break;
    case R_IFS0 ... R_IFS0 + 0x7F:
        i = (addr - R_IFS0) / 0x10;
        if (i == 0) {
            pic32_evic_clear_core_sw(s, s->ifs[0] & ~value);
        }
        /* Held sources are the peripheral's to withdraw, not software's. */
        s->ifs[i] = value & ~s->level[i];
        pic32_evic_update(s);
        break;
    case R_IEC0 ... R_IEC0 + 0x7F:
        s->iec[(addr - R_IEC0) / 0x10] = value;
        pic32_evic_update(s);
        break;
    case R_IPC0 ... R_IPC0 + 0x3FF:
        s->ipc[(addr - R_IPC0) / 0x10] = value;
        pic32_evic_update(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-evic: write of 0x%08x to 0x%03x\n",
                      value, (unsigned)addr);
        break;
    }
}

static uint32_t pic32_evic_off_read(void *opaque, hwaddr addr)
{
    PIC32EvicState *s = opaque;

    return s->offset[addr / 4];
}

static void pic32_evic_off_write(void *opaque, hwaddr addr, uint32_t value)
{
    PIC32EvicState *s = opaque;

    s->offset[addr / 4] = value & OFF_MASK;
    pic32_evic_update(s);
}

static const PIC32RegsOps pic32_evic_ctrl_ops = {
    .read = pic32_evic_ctrl_read,
    .write = pic32_evic_ctrl_write,
};

static const PIC32RegsOps pic32_evic_off_ops = {
    .read = pic32_evic_off_read,
    .write = pic32_evic_off_write,
};

static void pic32_evic_reset_hold(Object *obj, ResetType type)
{
    PIC32EvicState *s = PIC32_EVIC(obj);

    s->intcon = 0;
    s->priss = 0;
    s->iptmr = 0;
    memset(s->ifs, 0, sizeof(s->ifs));
    memset(s->iec, 0, sizeof(s->iec));
    memset(s->ipc, 0, sizeof(s->ipc));
    memset(s->offset, 0, sizeof(s->offset));
    memset(s->level, 0, sizeof(s->level));
    s->requested_ripl = 0;
    s->requested_offset = 0;
    s->requested_src = 0;
    cpu_mips_eic_request(s->cpu, 0, 0);
}

static void pic32_evic_realize(DeviceState *dev, Error **errp)
{
    PIC32EvicState *s = PIC32_EVIC(dev);

    if (!s->cpu) {
        error_setg(errp, "pic32-evic: no CPU to interrupt");
        return;
    }

    pic32_regs_init_io(&s->ctrl, OBJECT(dev), &pic32_evic_ctrl_ops, s,
                       "pic32-evic", PIC32_EVIC_CTRL_SIZE,
                       PIC32_REGS_ALIASED);
    pic32_regs_init_io(&s->off, OBJECT(dev), &pic32_evic_off_ops, s,
                       "pic32-evic-off", PIC32_EVIC_OFF_SIZE,
                       PIC32_REGS_PLAIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->ctrl);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->off);

    qdev_init_gpio_in_named(dev, pic32_evic_set_irq, PIC32_EVIC_IRQ_GPIO,
                            PIC32_EVIC_SOURCES);
    qdev_init_gpio_in_named(dev, pic32_evic_set_irq_level,
                            PIC32_EVIC_IRQ_LEVEL_GPIO, PIC32_EVIC_SOURCES);
}

static const Property pic32_evic_properties[] = {
    DEFINE_PROP_LINK("cpu", PIC32EvicState, cpu, TYPE_MIPS_CPU, MIPSCPU *),
};

static const VMStateDescription pic32_evic_vmstate = {
    .name = "pic32-evic",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(intcon, PIC32EvicState),
        VMSTATE_UINT32(priss, PIC32EvicState),
        VMSTATE_UINT32(iptmr, PIC32EvicState),
        VMSTATE_UINT32_ARRAY(ifs, PIC32EvicState, PIC32_EVIC_IFS_REGS),
        VMSTATE_UINT32_ARRAY(iec, PIC32EvicState, PIC32_EVIC_IFS_REGS),
        VMSTATE_UINT32_ARRAY(ipc, PIC32EvicState, PIC32_EVIC_IPC_REGS),
        VMSTATE_UINT32_ARRAY(offset, PIC32EvicState, PIC32_EVIC_SOURCES),
        VMSTATE_UINT32_ARRAY(level, PIC32EvicState, PIC32_EVIC_IFS_REGS),
        VMSTATE_UINT32(requested_ripl, PIC32EvicState),
        VMSTATE_UINT32(requested_offset, PIC32EvicState),
        VMSTATE_UINT32(requested_src, PIC32EvicState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic32_evic_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic32_evic_realize;
    dc->vmsd = &pic32_evic_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, pic32_evic_properties);
    rc->phases.hold = pic32_evic_reset_hold;
}

static const TypeInfo pic32_evic_types[] = {
    {
        .name = TYPE_PIC32_EVIC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC32EvicState),
        .class_init = pic32_evic_class_init,
    },
};

DEFINE_TYPES(pic32_evic_types)
