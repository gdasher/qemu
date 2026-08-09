/*
 * PIC32 DMA controller
 *
 * Eight channels, each moving bytes between two physical addresses on its own
 * schedule. A channel is told a source and a destination, how big each of them
 * is, and how much to move at a time; it then moves one "cell" every time
 * something wakes it, and reports a block as done once the longer of the two
 * has been walked from end to end. The shorter one wraps as many times as it
 * takes, which is what makes the two interesting cases work with the same
 * counter: a 5400-byte buffer into a one-byte peripheral register, and a
 * two-byte peripheral register into a 9606-byte buffer.
 *
 * What wakes a channel is an interrupt source, named by number in DCHxECON.
 * The controller sees those before the interrupt controller has decided
 * whether anyone is listening: a channel armed with SIRQEN runs whether or not
 * IEC has the source enabled, which is exactly how firmware uses it -- the SPI
 * transmit source is left masked in IEC and drives nothing but the DMA.
 *
 * Transfers happen in one go rather than over the bus cycles they would really
 * take. Nothing in this machine can tell: a peripheral that is the source of a
 * cell has already been asked for the data by the time its interrupt fired,
 * and one that is the destination is written before the next trigger can
 * arrive.
 *
 * The CRC module is here too, because on this part it belongs to the DMA
 * rather than standing on its own: any one channel can have its data routed
 * through it, and the firmware then gets the CRC of a block for free while the
 * block moves. Only LFSR mode is modelled -- the IP-header mode is for
 * checksumming network headers and nothing here does that.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/pic32/pic32_dmac.h"
#include "hw/pic32/pic32_regs.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "qapi/error.h"

/* Global registers. */
enum {
    R_DMACON   = 0x00,
    R_DMASTAT  = 0x10,
    R_DMAADDR  = 0x20,
    R_DCRCCON  = 0x30,
    R_DCRCDATA = 0x40,
    R_DCRCXOR  = 0x50,
    R_CHANNELS = 0x60,
};

/* Within one channel's block. */
enum {
    R_CH_CON  = 0x00,
    R_CH_ECON = 0x10,
    R_CH_INT  = 0x20,
    R_CH_SSA  = 0x30,
    R_CH_DSA  = 0x40,
    R_CH_SSIZ = 0x50,
    R_CH_DSIZ = 0x60,
    R_CH_SPTR = 0x70,
    R_CH_DPTR = 0x80,
    R_CH_CSIZ = 0x90,
    R_CH_CPTR = 0xA0,
    R_CH_DAT  = 0xB0,
};

#define CH_STRIDE 0xC0

/* DMACON */
#define DMACON_ON       (1u << 15)
#define DMACON_SUSPEND  (1u << 12)
#define DMACON_DMABUSY  (1u << 11)

/* DCHxCON */
#define CHCON_CHBUSY    (1u << 15)
#define CHCON_CHPATLEN  (1u << 11)
#define CHCON_CHCHNS    (1u << 8)
#define CHCON_CHEN      (1u << 7)
#define CHCON_CHAED     (1u << 6)
#define CHCON_CHCHN     (1u << 5)
#define CHCON_CHAEN     (1u << 4)
#define CHCON_CHEDET    (1u << 2)

/* DCHxECON */
#define CHECON_CFORCE   (1u << 7)
#define CHECON_CABORT   (1u << 6)
#define CHECON_PATEN    (1u << 5)
#define CHECON_SIRQEN   (1u << 4)
#define CHECON_AIRQEN   (1u << 3)
#define CHECON_SIRQ(v)  (((v) >> 8) & 0xFF)
#define CHECON_AIRQ(v)  (((v) >> 16) & 0xFF)

/* DCHxINT: flags in the low byte, enables sixteen bits above them. */
#define CHINT_CHERIF    (1u << 0)
#define CHINT_CHTAIF    (1u << 1)
#define CHINT_CHCCIF    (1u << 2)
#define CHINT_CHBCIF    (1u << 3)
#define CHINT_CHDHIF    (1u << 4)
#define CHINT_CHDDIF    (1u << 5)
#define CHINT_CHSHIF    (1u << 6)
#define CHINT_CHSDIF    (1u << 7)
#define CHINT_ENABLE_SHIFT 16

/* DCRCCON */
#define DCRCCON_CRCCH(v)  ((v) & 0x7)
#define DCRCCON_CRCTYP    (1u << 5)
#define DCRCCON_CRCAPP    (1u << 6)
#define DCRCCON_CRCEN     (1u << 7)
#define DCRCCON_PLEN(v)   (((v) >> 8) & 0x1F)
#define DCRCCON_BITO      (1u << 24)

/*
 * A DMA address register holds a physical address. Firmware converts with
 * KVA_TO_PA before it writes one, so anything with KSEG bits set is a mistake
 * worth reporting rather than quietly masking -- it is the difference between
 * a transfer that lands where the guest meant and one that lands 0x20000000
 * away, and that is a bug this model exists to catch.
 */
static bool pic32_dmac_addr_ok(unsigned ch, const char *what, uint32_t addr)
{
    if (addr >= 0x80000000) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pic32-dmac: channel %u %s address 0x%08x is virtual; "
                      "DMA takes physical addresses (use KVA_TO_PA)\n",
                      ch, what, addr);
        return false;
    }
    return true;
}

static void pic32_dmac_update_irq(PIC32DmacState *s, unsigned ch)
{
    uint32_t intr = s->ch[ch].intr;
    bool active = (intr & 0xFF) & ((intr >> CHINT_ENABLE_SHIFT) & 0xFF);

    qemu_set_irq(s->irq[ch], active);
}

static void pic32_dmac_flag(PIC32DmacState *s, unsigned ch, uint32_t flag)
{
    s->ch[ch].intr |= flag;
    pic32_dmac_update_irq(s, ch);
}

/*
 * One byte through the CRC. LFSR mode: the register is PLEN+1 bits wide, data
 * goes in a bit at a time, and the polynomial in DCRCXOR is applied whenever
 * the bit shifted off the top was set. BITO chooses which end of the byte goes
 * first; the firmware's CRC-16/CCITT feeds the most significant bit first,
 * which is BITO clear.
 */
static void pic32_dmac_crc_byte(PIC32DmacState *s, uint8_t byte)
{
    unsigned plen = DCRCCON_PLEN(s->dcrccon) + 1;
    uint32_t mask = plen >= 32 ? 0xFFFFFFFF : ((1u << plen) - 1);
    uint32_t top = 1u << (plen - 1);
    bool lsb_first = s->dcrccon & DCRCCON_BITO;
    unsigned i;

    for (i = 0; i < 8; i++) {
        unsigned bit = lsb_first ? i : 7 - i;
        bool in = (byte >> bit) & 1;
        bool out = (s->dcrcdata & top) != 0;

        s->dcrcdata = (s->dcrcdata << 1) & mask;
        if (out ^ in) {
            s->dcrcdata ^= s->dcrcxor & mask;
        }
    }
}

static bool pic32_dmac_crc_on(PIC32DmacState *s, unsigned ch)
{
    return (s->dcrccon & DCRCCON_CRCEN) && DCRCCON_CRCCH(s->dcrccon) == ch &&
           !(s->dcrccon & DCRCCON_CRCTYP);
}

/*
 * How long a block is. Source and destination are walked together and the
 * shorter of them wraps, so the block ends when the longer one has been
 * covered once. A size of zero means 256 on this part.
 */
static uint32_t pic32_dmac_size(uint32_t siz)
{
    return siz ? siz : 256;
}

static uint32_t pic32_dmac_block_len(PIC32DmacChannel *c)
{
    uint32_t ssiz = pic32_dmac_size(c->ssiz);
    uint32_t dsiz = pic32_dmac_size(c->dsiz);

    return MAX(ssiz, dsiz);
}

/* Moves one cell. Returns true if that finished the block. */
static bool pic32_dmac_cell(PIC32DmacState *s, unsigned ch)
{
    PIC32DmacChannel *c = &s->ch[ch];
    uint32_t ssiz = pic32_dmac_size(c->ssiz);
    uint32_t dsiz = pic32_dmac_size(c->dsiz);
    uint32_t csiz = pic32_dmac_size(c->csiz);
    uint32_t len = pic32_dmac_block_len(c);
    bool crc = pic32_dmac_crc_on(s, ch);
    uint32_t i;

    if (!pic32_dmac_addr_ok(ch, "source", c->ssa) ||
        !pic32_dmac_addr_ok(ch, "destination", c->dsa)) {
        pic32_dmac_flag(s, ch, CHINT_CHERIF);
        c->con &= ~CHCON_CHEN;
        return true;
    }

    c->con |= CHCON_CHBUSY;

    for (i = 0; i < csiz && c->cptr < len; i++, c->cptr++) {
        uint8_t byte;

        address_space_read(&address_space_memory, c->ssa + c->sptr,
                           MEMTXATTRS_UNSPECIFIED, &byte, 1);
        if (crc) {
            pic32_dmac_crc_byte(s, byte);
        }
        /*
         * With the CRC appending its result the destination gets the CRC
         * rather than the data; nothing here uses that, so the data goes
         * through either way and CRCAPP is only honoured to the extent of
         * saying so.
         */
        address_space_write(&address_space_memory, c->dsa + c->dptr,
                            MEMTXATTRS_UNSPECIFIED, &byte, 1);

        c->sptr = (c->sptr + 1) % ssiz;
        c->dptr = (c->dptr + 1) % dsiz;
    }

    c->con &= ~CHCON_CHBUSY;

    if (c->cptr >= len) {
        c->cptr = 0;
        c->sptr = 0;
        c->dptr = 0;
        pic32_dmac_flag(s, ch, CHINT_CHBCIF);
        if (!(c->con & CHCON_CHAEN)) {
            c->con &= ~CHCON_CHEN;
        }
        return true;
    }

    pic32_dmac_flag(s, ch, CHINT_CHCCIF);
    return false;
}

static void pic32_dmac_trigger(PIC32DmacState *s, unsigned ch)
{
    PIC32DmacChannel *c = &s->ch[ch];

    if (!(s->dmacon & DMACON_ON) || (s->dmacon & DMACON_SUSPEND)) {
        return;
    }
    if (!(c->con & CHCON_CHEN)) {
        return;
    }

    if (pic32_dmac_cell(s, ch)) {
        /*
         * Chaining: a channel with CHCHN set is enabled by its neighbour
         * finishing. CHCHNS picks which neighbour -- the channel above when
         * set, the one below when clear.
         */
        unsigned n;

        for (n = 0; n < PIC32_DMAC_CHANNELS; n++) {
            PIC32DmacChannel *o = &s->ch[n];
            unsigned from = (o->con & CHCON_CHCHNS) ? n + 1 : n - 1;

            if ((o->con & CHCON_CHCHN) && from == ch) {
                o->con |= CHCON_CHEN;
            }
        }
    }
}

void pic32_dmac_irq_event(PIC32DmacState *s, unsigned source, bool level)
{
    unsigned ch;

    if (!level) {
        return;
    }

    for (ch = 0; ch < PIC32_DMAC_CHANNELS; ch++) {
        PIC32DmacChannel *c = &s->ch[ch];

        if ((c->econ & CHECON_SIRQEN) && CHECON_SIRQ(c->econ) == source) {
            pic32_dmac_trigger(s, ch);
        }
        if ((c->econ & CHECON_AIRQEN) && CHECON_AIRQ(c->econ) == source) {
            c->con &= ~CHCON_CHEN;
            pic32_dmac_flag(s, ch, CHINT_CHTAIF);
        }
    }
}

static uint32_t pic32_dmac_read(void *opaque, hwaddr offset)
{
    PIC32DmacState *s = opaque;
    unsigned ch;

    switch (offset) {
    case R_DMACON:
        return s->dmacon;
    case R_DMASTAT:
        return s->dmastat;
    case R_DMAADDR:
        return s->dmaaddr;
    case R_DCRCCON:
        return s->dcrccon;
    case R_DCRCDATA:
        return s->dcrcdata;
    case R_DCRCXOR:
        return s->dcrcxor;
    }

    if (offset < R_CHANNELS) {
        return 0;
    }
    ch = (offset - R_CHANNELS) / CH_STRIDE;
    if (ch >= PIC32_DMAC_CHANNELS) {
        return 0;
    }

    switch ((offset - R_CHANNELS) % CH_STRIDE) {
    case R_CH_CON:
        return s->ch[ch].con;
    case R_CH_ECON:
        return s->ch[ch].econ;
    case R_CH_INT:
        return s->ch[ch].intr;
    case R_CH_SSA:
        return s->ch[ch].ssa;
    case R_CH_DSA:
        return s->ch[ch].dsa;
    case R_CH_SSIZ:
        return s->ch[ch].ssiz;
    case R_CH_DSIZ:
        return s->ch[ch].dsiz;
    case R_CH_SPTR:
        return s->ch[ch].sptr;
    case R_CH_DPTR:
        return s->ch[ch].dptr;
    case R_CH_CSIZ:
        return s->ch[ch].csiz;
    case R_CH_CPTR:
        return s->ch[ch].cptr;
    case R_CH_DAT:
        return s->ch[ch].dat;
    }
    return 0;
}

static void pic32_dmac_write(void *opaque, hwaddr offset, uint32_t value)
{
    PIC32DmacState *s = opaque;
    unsigned ch;

    switch (offset) {
    case R_DMACON:
        s->dmacon = value & (DMACON_ON | DMACON_SUSPEND);
        return;
    case R_DMASTAT:
        return;                 /* read-only */
    case R_DMAADDR:
        s->dmaaddr = value;
        return;
    case R_DCRCCON:
        s->dcrccon = value;
        return;
    case R_DCRCDATA:
        s->dcrcdata = value;
        return;
    case R_DCRCXOR:
        s->dcrcxor = value;
        return;
    }

    if (offset < R_CHANNELS) {
        return;
    }
    ch = (offset - R_CHANNELS) / CH_STRIDE;
    if (ch >= PIC32_DMAC_CHANNELS) {
        return;
    }

    switch ((offset - R_CHANNELS) % CH_STRIDE) {
    case R_CH_CON:
        s->ch[ch].con = (s->ch[ch].con & CHCON_CHBUSY) | (value & ~CHCON_CHBUSY);
        return;
    case R_CH_ECON:
        s->ch[ch].econ = value;
        if (value & CHECON_CABORT) {
            s->ch[ch].econ &= ~CHECON_CABORT;
            s->ch[ch].con &= ~CHCON_CHEN;
            s->ch[ch].cptr = 0;
            s->ch[ch].sptr = 0;
            s->ch[ch].dptr = 0;
        }
        if (value & CHECON_CFORCE) {
            s->ch[ch].econ &= ~CHECON_CFORCE;
            pic32_dmac_trigger(s, ch);
        }
        return;
    case R_CH_INT:
        s->ch[ch].intr = value;
        pic32_dmac_update_irq(s, ch);
        return;
    case R_CH_SSA:
        s->ch[ch].ssa = value;
        return;
    case R_CH_DSA:
        s->ch[ch].dsa = value;
        return;
    case R_CH_SSIZ:
        s->ch[ch].ssiz = value & 0xFFFF;
        return;
    case R_CH_DSIZ:
        s->ch[ch].dsiz = value & 0xFFFF;
        return;
    case R_CH_SPTR:
    case R_CH_DPTR:
    case R_CH_CPTR:
        return;                 /* read-only */
    case R_CH_CSIZ:
        s->ch[ch].csiz = value & 0xFFFF;
        return;
    case R_CH_DAT:
        s->ch[ch].dat = value;
        return;
    }
}

static const PIC32RegsOps pic32_dmac_ops = {
    .read = pic32_dmac_read,
    .write = pic32_dmac_write,
};

static void pic32_dmac_reset_hold(Object *obj, ResetType type)
{
    PIC32DmacState *s = PIC32_DMAC(obj);
    unsigned ch;

    s->dmacon = 0;
    s->dmastat = 0;
    s->dmaaddr = 0;
    s->dcrccon = 0;
    s->dcrcdata = 0;
    s->dcrcxor = 0;
    memset(s->ch, 0, sizeof(s->ch));
    for (ch = 0; ch < PIC32_DMAC_CHANNELS; ch++) {
        qemu_set_irq(s->irq[ch], 0);
    }
}

static void pic32_dmac_realize(DeviceState *dev, Error **errp)
{
    PIC32DmacState *s = PIC32_DMAC(dev);

    qdev_init_gpio_out_named(dev, s->irq, PIC32_DMAC_IRQ_GPIO,
                             PIC32_DMAC_CHANNELS);
}

static void pic32_dmac_init(Object *obj)
{
    PIC32DmacState *s = PIC32_DMAC(obj);

    pic32_regs_init_io(&s->iomem, obj, &pic32_dmac_ops, s, "pic32-dmac",
                       PIC32_DMAC_SIZE, PIC32_REGS_ALIASED);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_pic32_dmac_channel = {
    .name = "pic32-dmac-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(con, PIC32DmacChannel),
        VMSTATE_UINT32(econ, PIC32DmacChannel),
        VMSTATE_UINT32(intr, PIC32DmacChannel),
        VMSTATE_UINT32(ssa, PIC32DmacChannel),
        VMSTATE_UINT32(dsa, PIC32DmacChannel),
        VMSTATE_UINT32(ssiz, PIC32DmacChannel),
        VMSTATE_UINT32(dsiz, PIC32DmacChannel),
        VMSTATE_UINT32(sptr, PIC32DmacChannel),
        VMSTATE_UINT32(dptr, PIC32DmacChannel),
        VMSTATE_UINT32(csiz, PIC32DmacChannel),
        VMSTATE_UINT32(cptr, PIC32DmacChannel),
        VMSTATE_UINT32(dat, PIC32DmacChannel),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_pic32_dmac = {
    .name = "pic32-dmac",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dmacon, PIC32DmacState),
        VMSTATE_UINT32(dmastat, PIC32DmacState),
        VMSTATE_UINT32(dmaaddr, PIC32DmacState),
        VMSTATE_UINT32(dcrccon, PIC32DmacState),
        VMSTATE_UINT32(dcrcdata, PIC32DmacState),
        VMSTATE_UINT32(dcrcxor, PIC32DmacState),
        VMSTATE_STRUCT_ARRAY(ch, PIC32DmacState, PIC32_DMAC_CHANNELS, 1,
                             vmstate_pic32_dmac_channel, PIC32DmacChannel),
        VMSTATE_END_OF_LIST()
    },
};

static void pic32_dmac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = pic32_dmac_realize;
    dc->vmsd = &vmstate_pic32_dmac;
    rc->phases.hold = pic32_dmac_reset_hold;
}

static const TypeInfo pic32_dmac_types[] = {
    {
        .name = TYPE_PIC32_DMAC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PIC32DmacState),
        .instance_init = pic32_dmac_init,
        .class_init = pic32_dmac_class_init,
    },
};

DEFINE_TYPES(pic32_dmac_types)
