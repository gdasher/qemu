/*
 * PIC32 parallel master port, master mode
 *
 * The port drives an address bus and moves one 8- or 16-bit word at a time to
 * or from whatever is wired to it. This firmware uses it for an external SRAM
 * that buffers movie frames: it sets an address, then writes or reads several
 * thousand words with the address auto-incrementing, polling PMMODE.BUSY after
 * each one.
 *
 * The read pipeline is the part that has to be right. Reading the data
 * register hands back what the *previous* read cycle fetched and starts the
 * next one, so the guest always gets a word it asked for an access ago. The
 * firmware knows this and throws the first read away:
 *
 *     volatile uint16_t ignored = PMRDIN;
 *
 * A model that returned the addressed word immediately would shift every
 * buffer by one, and the firmware would not crash: it CRCs each frame and
 * would simply reject every one of them, which looks like a fault anywhere
 * else in the machine.
 *
 * BUSY always reads clear. A transfer here is instantaneous, and the guest
 * polls that bit after every word.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/pic32/pic32_pmp.h"
#include "hw/pic32/pic32_regs.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

enum {
    R_CON   = 0x00,
    R_MODE  = 0x10,
    R_ADDR  = 0x20,
    R_DOUT  = 0x30,
    R_DIN   = 0x40,
    R_AEN   = 0x50,
    R_STAT  = 0x60,
    R_WADDR = 0x70,
    R_RADDR = 0x80,
    R_RDIN  = 0x90,
};

/* PMCON */
#define CON_ON      (1u << 15)
#define CON_EXADR   (1u << 6)

/* PMMODE */
#define MODE_MODE_SHIFT 8
#define MODE_MODE       (3u << MODE_MODE_SHIFT)
#define MODE_MODE16     (1u << 10)
#define MODE_INCM_SHIFT 11
#define MODE_INCM       (3u << MODE_INCM_SHIFT)
#define MODE_IRQM_SHIFT 13
#define MODE_IRQM       (3u << MODE_IRQM_SHIFT)
#define MODE_BUSY       (1u << 15)

/* The address bus is 24 lines wide when the extended ones are enabled. */
#define ADDR_MASK 0x00FFFFFF

/* INCM: 1 increments the address after each transfer, 2 decrements it. */
enum { INCM_NONE = 0, INCM_UP = 1, INCM_DOWN = 2, INCM_SLAVE = 3 };

void pic32_pmp_attach(PIC32PmpState *s, const PIC32PmpTarget *target,
                      void *opaque)
{
    s->target = target;
    s->target_opaque = opaque;
}

static uint32_t pic32_pmp_advance(PIC32PmpState *s, uint32_t addr)
{
    switch ((s->mode & MODE_INCM) >> MODE_INCM_SHIFT) {
    case INCM_UP:
        return (addr + 1) & ADDR_MASK;
    case INCM_DOWN:
        return (addr - 1) & ADDR_MASK;
    default:
        return addr;
    }
}

/*
 * A completed transfer raises the interrupt if the mode asks for one -- but
 * not from inside the register access that completed it. What listens to this
 * source is a DMA channel whose next act is to read the data register, and
 * doing that from within a read of the same register is a re-entry the bus
 * would never make. The cycle ends, and then the interrupt goes out.
 */
static void pic32_pmp_irq_bh(void *opaque)
{
    PIC32PmpState *s = opaque;
    uint32_t n = s->irq_pending;

    /*
     * One pulse per completed cycle. Hardware raises the source once per
     * cycle and the DMA controller counts on seeing every one -- a chained
     * transfer moves one word per event, so an event swallowed here is a
     * transfer that stops one word short and never finishes.
     */
    s->irq_pending = 0;
    while (n--) {
        qemu_irq_pulse(s->irq);
    }
}

static void pic32_pmp_done(PIC32PmpState *s)
{
    if (s->mode & MODE_IRQM) {
        s->irq_pending++;
        qemu_bh_schedule(s->irq_bh);
    }
}

static uint16_t pic32_pmp_target_read(PIC32PmpState *s, uint32_t addr)
{
    if (!s->target) {
        /*
         * An address bus with nothing on it floats, and a board that forgot to
         * fit the memory should say so once rather than hand back a plausible
         * zero for the several thousand words that follow.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pic32-pmp: read of 0x%06x with nothing wired to the "
                      "port\n", addr);
        return 0xFFFF;
    }
    return s->target->read(s->target_opaque, addr);
}

static void pic32_pmp_target_write(PIC32PmpState *s, uint32_t addr,
                                   uint16_t data)
{
    if (s->target) {
        s->target->write(s->target_opaque, addr, data);
    }
}

static uint32_t pic32_pmp_read(void *opaque, hwaddr offset)
{
    PIC32PmpState *s = opaque;
    uint32_t value;

    switch (offset) {
    case R_CON:
        return s->con;
    case R_MODE:
        /* BUSY is never set: the transfer is over before this can be read. */
        return s->mode & ~MODE_BUSY;
    case R_ADDR:
        return s->addr;
    case R_DOUT:
        return s->dout;
    case R_DIN:
        /*
         * The legacy read register. It shares the buffer with PMRDIN but
         * starting a cycle from it as well would take the guest's two views of
         * one pipeline out of step, so this only looks.
         */
        return s->din;
    case R_AEN:
        return s->aen;
    case R_STAT:
        return s->stat;
    case R_WADDR:
        return s->waddr;
    case R_RADDR:
        return s->raddr;
    case R_RDIN:
        if (!(s->con & CON_ON)) {
            return s->din;
        }
        value = s->din;
        s->din = pic32_pmp_target_read(s, s->raddr);
        s->raddr = pic32_pmp_advance(s, s->raddr);
        pic32_pmp_done(s);
        return value;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-pmp: read of 0x%02x\n",
                      (unsigned)offset);
        return 0;
    }
}

static void pic32_pmp_write(void *opaque, hwaddr offset, uint32_t value)
{
    PIC32PmpState *s = opaque;

    switch (offset) {
    case R_CON:
        s->con = value;
        break;
    case R_MODE:
        if (((value & MODE_MODE) >> MODE_MODE_SHIFT) != 2) {
            qemu_log_mask(LOG_UNIMP,
                          "pic32-pmp: only master mode 2 is modelled\n");
        }
        s->mode = value & ~MODE_BUSY;
        break;
    case R_ADDR:
        /* Writing the shared address register sets both directions. */
        s->addr = value & ADDR_MASK;
        s->waddr = s->addr;
        s->raddr = s->addr;
        break;
    case R_DOUT:
        if (!(s->con & CON_ON)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pic32-pmp: write with the port off\n");
            break;
        }
        s->dout = value;
        pic32_pmp_target_write(s, s->waddr,
                               (s->mode & MODE_MODE16) ? value & 0xFFFF
                                                       : value & 0xFF);
        s->waddr = pic32_pmp_advance(s, s->waddr);
        pic32_pmp_done(s);
        break;
    case R_DIN:
        s->din = value;
        break;
    case R_AEN:
        s->aen = value;
        break;
    case R_STAT:
        s->stat = value;
        break;
    case R_WADDR:
        s->waddr = value & ADDR_MASK;
        break;
    case R_RADDR:
        s->raddr = value & ADDR_MASK;
        break;
    case R_RDIN:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "pic32-pmp: write of 0x%08x to 0x%02x\n",
                      value, (unsigned)offset);
        break;
    }
}

static const PIC32RegsOps pic32_pmp_regs_ops = {
    .read = pic32_pmp_read,
    .write = pic32_pmp_write,
};

static void pic32_pmp_reset_hold(Object *obj, ResetType type)
{
    PIC32PmpState *s = PIC32_PMP(obj);

    s->con = 0;
    s->mode = 0;
    s->addr = 0;
    s->aen = 0;
    s->stat = 0;
    s->waddr = 0;
    s->raddr = 0;
    s->dout = 0;
    s->din = 0;
}

static void pic32_pmp_realize(DeviceState *dev, Error **errp)
{
    PIC32PmpState *s = PIC32_PMP(dev);

    pic32_regs_init_io(&s->mmio, OBJECT(dev), &pic32_pmp_regs_ops, s,
                       "pic32-pmp", PIC32_PMP_SIZE, PIC32_REGS_ALIASED);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    s->irq_bh = qemu_bh_new(pic32_pmp_irq_bh, s);
    qdev_init_gpio_out_named(dev, &s->irq, PIC32_PMP_IRQ_GPIO, 1);
}

static const VMStateDescription pic32_pmp_vmstate = {
    .name = "pic32-pmp",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(con, PIC32PmpState),
        VMSTATE_UINT32(mode, PIC32PmpState),
        VMSTATE_UINT32(addr, PIC32PmpState),
        VMSTATE_UINT32(aen, PIC32PmpState),
        VMSTATE_UINT32(stat, PIC32PmpState),
        VMSTATE_UINT32(waddr, PIC32PmpState),
        VMSTATE_UINT32(raddr, PIC32PmpState),
        VMSTATE_UINT32(dout, PIC32PmpState),
        VMSTATE_UINT32(din, PIC32PmpState),
        VMSTATE_UINT32(irq_pending, PIC32PmpState),
        VMSTATE_END_OF_LIST()
    }
};

static void pic32_pmp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pic32_pmp_realize;
    dc->vmsd = &pic32_pmp_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = pic32_pmp_reset_hold;
}

static const TypeInfo pic32_pmp_types[] = {
    {
        .name = TYPE_PIC32_PMP,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC32PmpState),
        .class_init = pic32_pmp_class_init,
    },
};

DEFINE_TYPES(pic32_pmp_types)
