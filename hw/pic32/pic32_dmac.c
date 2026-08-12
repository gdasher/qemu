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
 * Batched transfers. Stepping one cell per source event is honest only if
 * delivering an event is cheap, and here it is not: a bottom-half or timer
 * ride through the main loop costs whatever the vCPU happens to be doing,
 * so a chained transfer ran at ~10 us of guest time per cell under load and
 * nearly nothing when idle -- a artifact of single-threaded emulation, where
 * silicon moves a word per bus cycle concurrently with the CPU. So for a
 * source the SoC declares to be a metronome (pic32_dmac_set_source_pacing --
 * the parallel port in master mode raises one event per cycle, a fixed time
 * apart), the whole block is moved on the first trigger and the completion
 * is delivered by a virtual-clock timer at exactly the time the bus cycles
 * would have taken. The guest cannot tell the data moved early: the pointer
 * registers are interpolated from the clock until the timer fires, the
 * completion flags and the chain wait for it, and the source's own events
 * during the flight are ignored -- they are the echoes of reads and writes
 * already made. Sources with no declared pacing (a baud-timed SPI shifter,
 * anything asynchronous) still step one cell per event, where the event
 * itself carries the timing.
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
#define CHCON_CHPIGN(v) (((v) >> 24) & 0xFF)
#define CHCON_CHBUSY    (1u << 15)
#define CHCON_CHPIGNEN  (1u << 13)
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

/* Both IRQ selects reset to 255, a source nothing raises. */
#define CHECON_RESET    0x00FFFF00

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
#define DCRCCON_WBO       (1u << 27)
#define DCRCCON_BYTO(v)   (((v) >> 28) & 0x3)

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
 * One byte through the CRC. LFSR mode: the register is PLEN+1 bits wide, each
 * data bit enters the bottom of the register, and the polynomial in DCRCXOR
 * is applied whenever the bit shifted off the top was set. BITO chooses which
 * end of the byte goes first.
 *
 * The bottom is the important word. This register divides the raw message,
 * so its result is not the textbook CRC of it: the textbook algorithms --
 * and every table implementation -- divide the message times x^n, which is
 * the same as XORing each input bit into the top of the register instead.
 * The two agree on nothing except the zero message, and this model shipping
 * the textbook form is exactly how firmware that validated frames here went
 * on to fail on silicon. A guest that wants CRC-16/CCITT out of this engine
 * has to seed it with 0xFFFF * x^-16 mod G (0x84CF) and shift sixteen zero
 * bits through the result.
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

        s->dcrcdata = ((s->dcrcdata << 1) | in) & mask;
        if (out) {
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
 * covered once. The size registers hold sixteen bits and the count is
 * one-based, so zero is the roll-over: it means 65536, not nothing.
 */
static uint32_t pic32_dmac_size(uint32_t siz)
{
    return siz ? siz : 65536;
}

static uint32_t pic32_dmac_block_len(PIC32DmacChannel *c)
{
    uint32_t ssiz = pic32_dmac_size(c->ssiz);
    uint32_t dsiz = pic32_dmac_size(c->dsiz);

    return MAX(ssiz, dsiz);
}

/*
 * How wide each access in a cell is. It matters: a peripheral register can
 * have side effects on every read -- the parallel port hands back one word and
 * fetches the next -- so reading it as two bytes fetches two words and keeps
 * half of each. The cell size is what says how much one event moves, and for
 * a peripheral that is the width of its register.
 */
static uint32_t pic32_dmac_unit(PIC32DmacChannel *c)
{
    uint32_t csiz = pic32_dmac_size(c->csiz);

    if (csiz % 4 == 0 && c->ssa % 4 == 0 && c->dsa % 4 == 0) {
        return 4;
    }
    if (csiz % 2 == 0 && c->ssa % 2 == 0 && c->dsa % 2 == 0) {
        return 2;
    }
    return 1;
}

/*
 * BYTO's four orderings of a 32-bit word, applied to the bytes on their way
 * into the CRC -- and, when WBO says so, on their way to the destination too.
 */
static void pic32_dmac_byto(uint8_t *w, unsigned order)
{
    uint8_t t;

    switch (order) {
    case 1:                     /* reverse the word: an endian swap */
        t = w[0]; w[0] = w[3]; w[3] = t;
        t = w[1]; w[1] = w[2]; w[2] = t;
        break;
    case 2:                     /* swap the half-words, bytes in place */
        t = w[0]; w[0] = w[2]; w[2] = t;
        t = w[1]; w[1] = w[3]; w[3] = t;
        break;
    case 3:                     /* reverse the bytes within each half-word */
        t = w[0]; w[0] = w[1]; w[1] = t;
        t = w[2]; w[2] = w[3]; w[3] = t;
        break;
    }
}

/*
 * One byte of the stream against the pattern. A one-byte pattern is the low
 * byte of CHPDAT; a two-byte pattern is two consecutive bytes of the stream
 * against CHPDAT, low byte first -- the order they would land in memory --
 * which is why the previous byte is kept: the pair can straddle two cells.
 * CHPIGN names a byte the comparison treats as a wildcard when CHPIGNEN is
 * set.
 */
static bool pic32_dmac_pat_match(PIC32DmacChannel *c, uint8_t byte, bool first)
{
    bool ignen = c->con & CHCON_CHPIGNEN;
    uint8_t ign = CHCON_CHPIGN(c->con);
    bool match;

    if (c->con & CHCON_CHPATLEN) {
        uint8_t prev = c->pat_prev;

        match = !first &&
                (prev == (c->dat & 0xFF) || (ignen && prev == ign)) &&
                (byte == ((c->dat >> 8) & 0xFF) || (ignen && byte == ign));
    } else {
        match = byte == (c->dat & 0xFF) || (ignen && byte == ign);
    }
    c->pat_prev = byte;
    return match;
}

typedef enum {
    CELL_MOVED,                 /* the block continues */
    CELL_BLOCK,                 /* this cell finished the block */
    CELL_ERROR,                 /* a bad address; nothing moved */
} PIC32DmacCellResult;

/*
 * Moves one cell: data and pointers only, no completion flags. The progress
 * flags -- source and destination reaching their midpoints and ends -- are
 * raised as they happen when live; a batched move leaves them for the timer,
 * like everything else the guest could see early.
 */
static PIC32DmacCellResult pic32_dmac_move_cell(PIC32DmacState *s,
                                                unsigned ch, bool live)
{
    PIC32DmacChannel *c = &s->ch[ch];
    uint32_t ssiz = pic32_dmac_size(c->ssiz);
    uint32_t dsiz = pic32_dmac_size(c->dsiz);
    uint32_t csiz = pic32_dmac_size(c->csiz);
    uint32_t unit = pic32_dmac_unit(c);
    bool crc = pic32_dmac_crc_on(s, ch);
    bool append = crc && (s->dcrccon & DCRCCON_CRCAPP);
    unsigned order = crc ? DCRCCON_BYTO(s->dcrccon) : 0;
    /*
     * With the CRC appending its result nothing is written while the block
     * moves, so the destination has no say in its length: the block is the
     * source, walked once.
     */
    uint32_t len = append ? ssiz : pic32_dmac_block_len(c);
    bool matched = false;
    uint32_t i;

    if (!pic32_dmac_addr_ok(ch, "source", c->ssa) ||
        !pic32_dmac_addr_ok(ch, "destination", c->dsa)) {
        return CELL_ERROR;
    }

    for (i = 0; i < csiz && c->cptr < len && !matched;) {
        uint8_t word[4], fed[4];
        uint32_t sold = c->sptr;
        uint32_t dold = c->dptr;
        uint32_t step, b;

        /*
         * Never touch more than what remains: of the cell, of the block, or
         * of either region before it wraps. A region shorter than the unit
         * gets a correctly narrowed access, not a wide one that trespasses,
         * and no access ever straddles a wrap.
         */
        step = MIN(unit, csiz - i);
        step = MIN(step, len - c->cptr);
        step = MIN(step, ssiz - c->sptr);
        if (!append) {
            step = MIN(step, dsiz - c->dptr);
        }

        address_space_read(&address_space_memory, c->ssa + c->sptr,
                           MEMTXATTRS_UNSPECIFIED, word, step);

        /*
         * The CRC sees the bytes in the order they land in memory unless
         * BYTO reorders them; the reorder is defined on whole words, so a
         * narrowed or unaligned access falls back to source order -- the
         * datasheet does not allow such transfers with WBO in any case.
         */
        memcpy(fed, word, step);
        if (order) {
            if (step == 4) {
                pic32_dmac_byto(fed, order);
            } else if (!s->byto_logged) {
                s->byto_logged = true;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "pic32-dmac: BYTO on a transfer not made of "
                              "aligned words; feeding source order\n");
            }
        }
        if (crc) {
            for (b = 0; b < step; b++) {
                pic32_dmac_crc_byte(s, fed[b]);
            }
        }
        if (c->econ & CHECON_PATEN) {
            for (b = 0; b < step && !matched; b++) {
                matched = pic32_dmac_pat_match(c, word[b], c->cptr + b == 0);
            }
        }
        if (!append) {
            address_space_write(&address_space_memory, c->dsa + c->dptr,
                                MEMTXATTRS_UNSPECIFIED,
                                (s->dcrccon & DCRCCON_WBO) && crc ? fed : word,
                                step);
        }

        c->sptr = (c->sptr + step) % ssiz;
        if (!append) {
            c->dptr = (c->dptr + step) % dsiz;
        }
        c->cptr += step;
        i += step;

        if (live) {
            if (sold + step == ssiz) {
                pic32_dmac_flag(s, ch, CHINT_CHSDIF);
            }
            if (sold < ssiz / 2 && sold + step >= ssiz / 2) {
                pic32_dmac_flag(s, ch, CHINT_CHSHIF);
            }
            if (!append) {
                if (dold + step == dsiz) {
                    pic32_dmac_flag(s, ch, CHINT_CHDDIF);
                }
                if (dold < dsiz / 2 && dold + step >= dsiz / 2) {
                    pic32_dmac_flag(s, ch, CHINT_CHDHIF);
                }
            }
        }
    }

    if (c->cptr >= len || matched) {
        if (append) {
            /*
             * The block's data went through the CRC and nowhere else; what
             * the destination gets, now that the block is done, is the
             * accumulated CRC itself: low byte first, as many bytes of it
             * as the destination is declared to hold, at most the
             * register's four.
             */
            uint8_t out[4] = {
                s->dcrcdata, s->dcrcdata >> 8,
                s->dcrcdata >> 16, s->dcrcdata >> 24,
            };

            address_space_write(&address_space_memory, c->dsa,
                                MEMTXATTRS_UNSPECIFIED, out, MIN(dsiz, 4));
        }
        c->cptr = 0;
        c->sptr = 0;
        c->dptr = 0;
        return CELL_BLOCK;
    }
    return CELL_MOVED;
}

static void pic32_dmac_error(PIC32DmacState *s, unsigned ch)
{
    pic32_dmac_flag(s, ch, CHINT_CHERIF);
    s->ch[ch].con &= ~CHCON_CHEN;
}

/*
 * Chaining: a channel with CHCHN set is enabled by its neighbour finishing.
 * CHCHNS picks which neighbour -- the channel above when set, the one below
 * when clear.
 */
static void pic32_dmac_chain(PIC32DmacState *s, unsigned ch)
{
    unsigned n;

    for (n = 0; n < PIC32_DMAC_CHANNELS; n++) {
        PIC32DmacChannel *o = &s->ch[n];
        unsigned from = (o->con & CHCON_CHCHNS) ? n + 1 : n - 1;

        if ((o->con & CHCON_CHCHN) && from == ch) {
            o->con |= CHCON_CHEN;
        }
    }
}

static void pic32_dmac_block_done(PIC32DmacState *s, unsigned ch)
{
    PIC32DmacChannel *c = &s->ch[ch];

    pic32_dmac_flag(s, ch, CHINT_CHBCIF);
    c->con &= ~CHCON_CHEDET;
    if (!(c->con & CHCON_CHAEN)) {
        c->con &= ~CHCON_CHEN;
    }
    pic32_dmac_chain(s, ch);
}

/*
 * How many cells of a flight the clock says have happened, for the pointer
 * registers: read-only diagnostics on real hardware, so what matters is that
 * they advance through the block over the time the block takes.
 */
static uint32_t pic32_dmac_batch_cells_done(const PIC32DmacChannel *c)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t per_cell;

    if (now >= c->batch_end_ns) {
        return c->batch_cells;
    }
    per_cell = (c->batch_end_ns - c->batch_start_ns) / c->batch_cells;
    if (per_cell <= 0) {
        return c->batch_cells;
    }
    return MIN((uint32_t)((now - c->batch_start_ns) / per_cell),
               c->batch_cells);
}

/* A pointer register during a flight, interpolated from the clock. */
static uint32_t pic32_dmac_batch_ptr(const PIC32DmacChannel *c, uint32_t siz)
{
    uint64_t moved = (uint64_t)pic32_dmac_batch_cells_done(c) *
                     pic32_dmac_size(c->csiz);

    return moved % pic32_dmac_size(siz);
}

/* Ends a flight without completing it: CABORT, abort IRQ, DMA off. */
static void pic32_dmac_batch_cancel(PIC32DmacState *s, unsigned ch,
                                    bool keep_progress)
{
    PIC32DmacChannel *c = &s->ch[ch];

    if (!c->batch) {
        return;
    }
    if (keep_progress) {
        /*
         * An abort by interrupt leaves the pointers where the transfer
         * stopped -- the FRM says so, "allowing the user to recover" -- so
         * materialise what the clock says they were.
         */
        c->sptr = pic32_dmac_batch_ptr(c, c->ssiz);
        c->dptr = pic32_dmac_batch_ptr(c, c->dsiz);
        c->cptr = 0;
    }
    timer_del(c->timer);
    c->batch = false;
}

/* The timer: the bus cycles have elapsed, deliver the completion. */
static void pic32_dmac_batch_done(void *opaque)
{
    PIC32DmacChannel *c = opaque;
    PIC32DmacState *s = c->parent;

    if (!c->batch) {
        return;
    }
    c->batch = false;
    /*
     * Every cell completion, the final cell's included -- the cells before
     * the last would each have flagged it on the way -- and the progress
     * flags: a whole block walks both regions through their midpoints to
     * their ends, so all four fired somewhere during the flight.
     */
    pic32_dmac_flag(s, c->index,
                    CHINT_CHCCIF | CHINT_CHSHIF | CHINT_CHSDIF |
                    CHINT_CHDHIF | CHINT_CHDDIF);
    pic32_dmac_block_done(s, c->index);
}

/*
 * Moves the whole block now and schedules its completion for when the
 * paced source would have finished raising the events, one per cell.
 */
static void pic32_dmac_batch_start(PIC32DmacState *s, unsigned ch,
                                   uint32_t cycle_ns)
{
    PIC32DmacChannel *c = &s->ch[ch];
    uint32_t cells = 0;

    for (;;) {
        PIC32DmacCellResult r = pic32_dmac_move_cell(s, ch, false);

        if (r == CELL_ERROR) {
            pic32_dmac_error(s, ch);
            return;
        }
        cells++;
        if (r == CELL_BLOCK) {
            break;
        }
    }

    c->batch = true;
    c->batch_cells = cells;
    c->batch_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    c->batch_end_ns = c->batch_start_ns + (int64_t)cells * cycle_ns;
    timer_mod(c->timer, c->batch_end_ns);
}

static void pic32_dmac_trigger(PIC32DmacState *s, unsigned ch)
{
    PIC32DmacChannel *c = &s->ch[ch];
    uint32_t cycle_ns;

    if (!(s->dmacon & DMACON_ON) || (s->dmacon & DMACON_SUSPEND)) {
        return;
    }
    if (!(c->con & CHCON_CHEN)) {
        return;
    }
    if (c->batch) {
        /* The block is committed; these are the echoes of its own cycles. */
        return;
    }

    /*
     * A paced source means the events are a metronome, so the block's timing
     * is arithmetic: move it all now, deliver the completion on the clock.
     * Pattern match needs the data inspected as the events actually arrive,
     * and a CRC riding the channel needs its accumulator honest whenever the
     * guest peeks at it, so both stay on the stepping path (nothing here
     * declares pacing and does either; the log would say if something
     * started to).
     */
    cycle_ns = (c->econ & CHECON_SIRQEN) && !(c->econ & CHECON_PATEN) &&
               !pic32_dmac_crc_on(s, ch) ?
               s->pacing_ns[CHECON_SIRQ(c->econ)] : 0;
    if (cycle_ns) {
        pic32_dmac_batch_start(s, ch, cycle_ns);
        return;
    }

    switch (pic32_dmac_move_cell(s, ch, true)) {
    case CELL_ERROR:
        pic32_dmac_error(s, ch);
        break;
    case CELL_BLOCK:
        /* The final cell is still a cell; it completes too. */
        pic32_dmac_flag(s, ch, CHINT_CHCCIF);
        pic32_dmac_block_done(s, ch);
        break;
    case CELL_MOVED:
        pic32_dmac_flag(s, ch, CHINT_CHCCIF);
        break;
    }
}

void pic32_dmac_irq_event(PIC32DmacState *s, unsigned source, bool level,
                          bool pulsed)
{
    unsigned ch;

    if (!level) {
        return;
    }

    for (ch = 0; ch < PIC32_DMAC_CHANNELS; ch++) {
        PIC32DmacChannel *c = &s->ch[ch];

        /*
         * A disabled channel ignores its events unless CHAED says to
         * register them anyway; a start seen either way is what CHEDET
         * records.
         *
         * The phantom first cell: for a pulsed source, ignored is not
         * forgotten. The channel keeps one start event that arrived while
         * it was disabled and spends it the moment it is enabled, so
         * firmware that arms a channel and then forces the first cell gets
         * two of them. On silicon their port writes collide; here the
         * cells simply run in turn, so the data survives -- the timing
         * damage is the part this model does not reach. Measured on the
         * PIC32MK1024GPK100 against the parallel port. A level source's
         * line staying ready is not a discrete event to remember, so it is
         * not latched.
         */
        if ((c->econ & CHECON_SIRQEN) && CHECON_SIRQ(c->econ) == source) {
            if (c->con & (CHCON_CHEN | CHCON_CHAED)) {
                c->con |= CHCON_CHEDET;
            } else if (pulsed) {
                c->latched_start = true;
            }
            pic32_dmac_trigger(s, ch);
        }
        if ((c->econ & CHECON_AIRQEN) && CHECON_AIRQ(c->econ) == source &&
            (c->con & (CHCON_CHEN | CHCON_CHAED))) {
            pic32_dmac_batch_cancel(s, ch, true);
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
        /*
         * CHBUSY is not stored state: the channel is busy while it is
         * enabled or while a block is still in flight against the clock.
         */
        return (s->ch[ch].con & ~CHCON_CHBUSY) |
               (((s->ch[ch].con & CHCON_CHEN) || s->ch[ch].batch) ?
                CHCON_CHBUSY : 0);
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
        if (s->ch[ch].batch) {
            return pic32_dmac_batch_ptr(&s->ch[ch], s->ch[ch].ssiz);
        }
        return s->ch[ch].sptr;
    case R_CH_DPTR:
        if (s->ch[ch].batch) {
            return pic32_dmac_batch_ptr(&s->ch[ch], s->ch[ch].dsiz);
        }
        return s->ch[ch].dptr;
    case R_CH_CSIZ:
        return s->ch[ch].csiz;
    case R_CH_CPTR:
        /* Between transactions the cell pointer reads as zero, and a flight
           is always between transactions. */
        return s->ch[ch].batch ? 0 : s->ch[ch].cptr;
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
        if (!(s->dmacon & DMACON_ON)) {
            /* Turning the DMA off resets every channel's pointers, and with
               them any flight still against the clock. */
            for (ch = 0; ch < PIC32_DMAC_CHANNELS; ch++) {
                pic32_dmac_batch_cancel(s, ch, false);
                s->ch[ch].sptr = 0;
                s->ch[ch].dptr = 0;
                s->ch[ch].cptr = 0;
            }
        }
        return;
    case R_DMASTAT:
        return;                 /* read-only */
    case R_DMAADDR:
        s->dmaaddr = value;
        return;
    case R_DCRCCON:
        if ((value & DCRCCON_CRCAPP) && (value & DCRCCON_WBO)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pic32-dmac: WBO together with CRCAPP is not "
                          "allowed\n");
        }
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
    case R_CH_CON: {
        uint32_t ro = CHCON_CHBUSY | CHCON_CHEDET;
        bool enabling = (value & CHCON_CHEN) && !(s->ch[ch].con & CHCON_CHEN);

        /* Enabling the channel starts a fresh watch for events. */
        if (enabling) {
            s->ch[ch].con &= ~CHCON_CHEDET;
        }
        s->ch[ch].con = (s->ch[ch].con & ro) | (value & ~ro);
        if (enabling && s->ch[ch].latched_start) {
            /* The event that arrived while disabled fires now. */
            s->ch[ch].latched_start = false;
            s->ch[ch].con |= CHCON_CHEDET;
            pic32_dmac_trigger(s, ch);
        }
        return;
    }
    case R_CH_ECON:
        s->ch[ch].econ = value;
        if (value & CHECON_CABORT) {
            s->ch[ch].econ &= ~CHECON_CABORT;
            pic32_dmac_batch_cancel(s, ch, false);
            s->ch[ch].con &= ~(CHCON_CHEN | CHCON_CHEDET);
            s->ch[ch].cptr = 0;
            s->ch[ch].sptr = 0;
            s->ch[ch].dptr = 0;
        }
        if (value & CHECON_CFORCE) {
            s->ch[ch].econ &= ~CHECON_CFORCE;
            s->ch[ch].con |= CHCON_CHEDET;
            pic32_dmac_trigger(s, ch);
        }
        return;
    case R_CH_INT:
        s->ch[ch].intr = value;
        pic32_dmac_update_irq(s, ch);
        return;
    case R_CH_SSA:
        /*
         * Writing a start address resets the matching pointer (FRM 31.4.6);
         * clearing CHEN leaves the pointers alone, which is what makes a
         * suspended channel resumable. Software relies on the first half as
         * much as the second: Harmony sets up every transfer by rewriting
         * the addresses, so a channel whose last block was stopped partway
         * must start the new one from the top, not from where it gave up.
         * Progress through the block restarts with the pointer.
         *
         * Reconfiguring a channel whose block is still in flight is the
         * guest's mistake (the FRM says disable first); ending the flight
         * keeps the mistake from compounding into a completion delivered
         * against the new setup.
         */
        pic32_dmac_batch_cancel(s, ch, false);
        s->ch[ch].ssa = value;
        s->ch[ch].sptr = 0;
        s->ch[ch].cptr = 0;
        return;
    case R_CH_DSA:
        pic32_dmac_batch_cancel(s, ch, false);
        s->ch[ch].dsa = value;
        s->ch[ch].dptr = 0;
        s->ch[ch].cptr = 0;
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

void pic32_dmac_set_source_pacing(PIC32DmacState *s, unsigned source,
                                  uint32_t cycle_ns)
{
    assert(source < ARRAY_SIZE(s->pacing_ns));
    s->pacing_ns[source] = cycle_ns;
}

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
    s->byto_logged = false;
    for (ch = 0; ch < PIC32_DMAC_CHANNELS; ch++) {
        PIC32DmacChannel *c = &s->ch[ch];
        QEMUTimer *timer = c->timer;

        if (timer) {
            timer_del(timer);
        }
        memset(c, 0, sizeof(*c));
        c->timer = timer;
        c->parent = s;
        c->index = ch;
        c->econ = CHECON_RESET;
        qemu_set_irq(s->irq[ch], 0);
    }
}

static void pic32_dmac_realize(DeviceState *dev, Error **errp)
{
    PIC32DmacState *s = PIC32_DMAC(dev);
    unsigned ch;

    qdev_init_gpio_out_named(dev, s->irq, PIC32_DMAC_IRQ_GPIO,
                             PIC32_DMAC_CHANNELS);
    for (ch = 0; ch < PIC32_DMAC_CHANNELS; ch++) {
        s->ch[ch].parent = s;
        s->ch[ch].index = ch;
        s->ch[ch].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       pic32_dmac_batch_done, &s->ch[ch]);
    }
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
    .version_id = 4,
    .minimum_version_id = 4,
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
        VMSTATE_UINT32(pat_prev, PIC32DmacChannel),
        VMSTATE_BOOL(latched_start, PIC32DmacChannel),
        VMSTATE_BOOL(batch, PIC32DmacChannel),
        VMSTATE_UINT32(batch_cells, PIC32DmacChannel),
        VMSTATE_INT64(batch_start_ns, PIC32DmacChannel),
        VMSTATE_INT64(batch_end_ns, PIC32DmacChannel),
        VMSTATE_END_OF_LIST()
    },
};

static int pic32_dmac_post_load(void *opaque, int version_id)
{
    PIC32DmacState *s = opaque;
    unsigned ch;

    for (ch = 0; ch < PIC32_DMAC_CHANNELS; ch++) {
        if (s->ch[ch].batch) {
            timer_mod(s->ch[ch].timer, s->ch[ch].batch_end_ns);
        }
    }
    return 0;
}

static const VMStateDescription vmstate_pic32_dmac = {
    .name = "pic32-dmac",
    .version_id = 4,
    .minimum_version_id = 4,
    .post_load = pic32_dmac_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dmacon, PIC32DmacState),
        VMSTATE_UINT32(dmastat, PIC32DmacState),
        VMSTATE_UINT32(dmaaddr, PIC32DmacState),
        VMSTATE_UINT32(dcrccon, PIC32DmacState),
        VMSTATE_UINT32(dcrcdata, PIC32DmacState),
        VMSTATE_UINT32(dcrcxor, PIC32DmacState),
        VMSTATE_STRUCT_ARRAY(ch, PIC32DmacState, PIC32_DMAC_CHANNELS, 3,
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
