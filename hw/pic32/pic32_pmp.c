/*
 * PIC32 parallel master port, master mode
 *
 * The port drives an address bus and moves one 8- or 16-bit word at a time to
 * or from whatever is wired to it. This firmware uses it for an external SRAM
 * that buffers movie frames: it sets an address, then writes or reads several
 * thousand words with the address auto-incrementing, polling PMMODE.BUSY after
 * each one.
 *
 * PMCON.DUALBUF picks which registers run the port. Clear -- the reset state
 * and the legacy flow most PIC32 code uses -- one address in PMADDR serves
 * both directions and PMDIN is the data register for reads and writes alike;
 * the dual-buffer registers (PMRDIN, PMRADDR, PMWADDR) do nothing. Set, reads
 * run from PMRADDR through PMRDIN and writes from PMWADDR through PMDOUT.
 *
 * The read pipeline is the part that has to be right. Reading the data
 * register -- PMRDIN, or PMDIN with DUALBUF clear -- hands back what the
 * *previous* read cycle fetched and starts the next one, so the guest always
 * gets a word it asked for an access ago. The firmware knows this and throws
 * the first read away:
 *
 *     volatile uint16_t ignored = PMRDIN;
 *
 * (PMCON.RDSTART primes the same pipeline without the throwaway read: setting
 * it launches one read cycle, and hardware clears it when the cycle ends,
 * which in this model is before the write of PMCON returns.)
 *
 * A model that returned the addressed word immediately would shift every
 * buffer by one, and the firmware would not crash: it CRCs each frame and
 * would simply reject every one of them, which looks like a fault anywhere
 * else in the machine.
 *
 * The live address advances after every cycle and reads back advanced. It
 * wraps within the address field: when PMCON.CSF turns the top one or two
 * bus lines into chip selects, the carry out of the bits below never reaches
 * them, so a transfer cannot increment its way off its own chip.
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
#define CON_CSF_SHIFT 6
#define CON_CSF     (3u << CON_CSF_SHIFT)
#define CON_ON      (1u << 15)
#define CON_EXADR   (1u << 16)
#define CON_DUALBUF (1u << 17)
#define CON_RDSTART (1u << 23)

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

/*
 * CSF: 2 makes the top two bus lines chip selects, 1 only the upper one,
 * 0 leaves both as address lines; 3 is reserved.
 */
enum { CSF_ADDR = 0, CSF_CS2 = 1, CSF_CS2_CS1 = 2 };

void pic32_pmp_attach(PIC32PmpState *s, const PIC32PmpTarget *target,
                      void *opaque)
{
    s->target = target;
    s->target_opaque = opaque;
}

/*
 * The span of address bits the auto-increment counter walks. The bus is 16
 * lines wide, or 24 with EXADR set, and when CSF assigns the top one or two
 * lines to chip selects the counter stops short of them: a carry out of the
 * bits below must not flip a chip select and wander onto another device
 * mid-transfer. When CSF leaves them as address lines they count normally.
 */
static uint32_t pic32_pmp_addr_field(PIC32PmpState *s)
{
    unsigned width = (s->con & CON_EXADR) ? 24 : 16;

    switch ((s->con & CON_CSF) >> CON_CSF_SHIFT) {
    case CSF_CS2_CS1:
        width -= 2;
        break;
    case CSF_CS2:
        width -= 1;
        break;
    default:
        break;
    }
    return (1u << width) - 1;
}

static uint32_t pic32_pmp_advance(PIC32PmpState *s, uint32_t addr)
{
    uint32_t field = pic32_pmp_addr_field(s);

    switch ((s->mode & MODE_INCM) >> MODE_INCM_SHIFT) {
    case INCM_UP:
        return (addr & ~field) | ((addr + 1) & field);
    case INCM_DOWN:
        return (addr & ~field) | ((addr - 1) & field);
    default:
        return addr;
    }
}

/*
 * A completed transfer raises the interrupt event -- but not from inside the
 * register access that completed it. What listens to this source is a DMA
 * channel whose next act is to read the data register, and doing that from
 * within a read of the same register is a re-entry the bus would never make.
 * The cycle ends, and then the interrupt goes out.
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
    /*
     * The interrupt flag sets at the end of every master-mode strobe under
     * IRQM 0 and 1 alike -- with IRQM 0 firmware simply leaves the enable
     * clear and polls the flag, so the event must still go out for the DMA
     * pacing that watches this source. IRQM 2 raises events only in the
     * buffered slave modes and 3 is reserved: neither produces a master-mode
     * event.
     */
    if (((s->mode & MODE_IRQM) >> MODE_IRQM_SHIFT) >= 2) {
        return;
    }
    s->irq_pending++;
    qemu_bh_schedule(s->irq_bh);
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

/*
 * Run one read cycle: fetch the addressed word into the pipeline buffer,
 * advance the address it came from, and hand back what the buffer held --
 * the word the previous cycle fetched.
 */
static uint32_t pic32_pmp_read_cycle(PIC32PmpState *s, uint32_t *addr)
{
    uint32_t prev = s->din;
    uint16_t data = pic32_pmp_target_read(s, *addr);

    /* Eight data lines in 8-bit mode: the high byte never crosses the bus. */
    if (!(s->mode & MODE_MODE16)) {
        data &= 0xFF;
    }
    s->din = data;
    *addr = pic32_pmp_advance(s, *addr);
    pic32_pmp_done(s);
    return prev;
}

/* Run one write cycle: put the word on the bus and advance the address. */
static void pic32_pmp_write_cycle(PIC32PmpState *s, uint32_t *addr,
                                  uint32_t data)
{
    pic32_pmp_target_write(s, *addr, (s->mode & MODE_MODE16) ? data & 0xFFFF
                                                             : data & 0xFF);
    *addr = pic32_pmp_advance(s, *addr);
    pic32_pmp_done(s);
}

/* The dual-buffer registers do nothing with DUALBUF clear. Say so, once. */
static void pic32_pmp_note_dualbuf_off(PIC32PmpState *s, const char *reg)
{
    if (!s->noted_dualbuf_off) {
        s->noted_dualbuf_off = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pic32-pmp: %s accessed with DUALBUF=0; the "
                      "single-buffer port reads and writes through PMADDR "
                      "and PMDIN\n", reg);
    }
}

static uint32_t pic32_pmp_read(void *opaque, hwaddr offset)
{
    PIC32PmpState *s = opaque;

    switch (offset) {
    case R_CON:
        return s->con;
    case R_MODE:
        /* BUSY is never set: the transfer is over before this can be read. */
        return s->mode & ~MODE_BUSY;
    case R_ADDR:
        /* The live single-buffer address, advanced by every cycle it ran. */
        return s->addr;
    case R_DOUT:
        return s->dout;
    case R_DIN:
        if (!(s->con & CON_DUALBUF) && (s->con & CON_ON)) {
            /*
             * The single-buffer data register: reading it returns the word
             * the previous cycle fetched and launches the next one -- the
             * same pipeline PMRDIN runs in dual-buffer mode.
             */
            return pic32_pmp_read_cycle(s, &s->addr);
        }
        /*
         * In dual-buffer mode this register shares the buffer with PMRDIN,
         * but starting a cycle from it as well would take the guest's two
         * views of one pipeline out of step, so here it only looks.
         */
        return s->din;
    case R_AEN:
        return s->aen;
    case R_STAT:
        return s->stat;
    case R_WADDR:
        if (!(s->con & CON_DUALBUF)) {
            pic32_pmp_note_dualbuf_off(s, "PMWADDR");
        }
        return s->waddr;
    case R_RADDR:
        if (!(s->con & CON_DUALBUF)) {
            pic32_pmp_note_dualbuf_off(s, "PMRADDR");
        }
        return s->raddr;
    case R_RDIN:
        if (!(s->con & CON_DUALBUF)) {
            pic32_pmp_note_dualbuf_off(s, "PMRDIN");
            return s->din;
        }
        if (!(s->con & CON_ON)) {
            return s->din;
        }
        return pic32_pmp_read_cycle(s, &s->raddr);
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
        /*
         * RDSTART is a command, not a state bit: setting it starts a read
         * cycle to prime the pipeline, and hardware clears it when the cycle
         * ends. Cycles here end within the write, so the bit is never stored
         * and can never read back stuck at one.
         */
        s->con = value & ~CON_RDSTART;
        if ((value & CON_RDSTART) && (s->con & CON_ON)) {
            pic32_pmp_read_cycle(s, (s->con & CON_DUALBUF) ? &s->raddr
                                                           : &s->addr);
        }
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
        /* The register keeps the value even when no cycle comes of it. */
        s->dout = value;
        if (!(s->con & CON_DUALBUF)) {
            /* Not a cycle trigger in single-buffer mode; PMDIN is. */
            break;
        }
        if (!(s->con & CON_ON)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pic32-pmp: write with the port off\n");
            break;
        }
        pic32_pmp_write_cycle(s, &s->waddr, value);
        break;
    case R_DIN:
        s->din = value;
        if (s->con & CON_DUALBUF) {
            /* Only PMDOUT triggers writes in dual-buffer mode. */
            break;
        }
        if (!(s->con & CON_ON)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pic32-pmp: write with the port off\n");
            break;
        }
        pic32_pmp_write_cycle(s, &s->addr, value);
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
    s->noted_dualbuf_off = false;
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
