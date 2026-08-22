/*
 * FM Radio Transmitter SPI Controller (XMASNGFMv2)
 *
 * The slave side of the protocol in PROTOCOL.md of the XMASNGFMv2 firmware,
 * modelled far enough to tell a master whether it is feeding the module
 * correctly:
 *
 *  - the command parser, byte for byte, with the one-exchange pipeline
 *    delay on responses (the answer to a byte comes back during the next);
 *  - the sample queue, drained by a virtual-clock timer at the rate the
 *    slave's TMR0 actually runs (its period and postscaler round the
 *    rate, and the "clock-ppm" property puts the slave's oscillator off
 *    by that much), so a master that bursts sees FM_RESP_OVERFLOW and one
 *    that starves sees underrun, both in the log;
 *  - a per-byte cost on the receive path: the slave polls its SPI from the
 *    main loop and needs some microseconds per byte, so bytes closer than
 *    that are lost as SPI overrun, as they would be on the PIC16;
 *  - the audio itself, if the "audiodev" property names a backend: what the
 *    ISR pops in FM audio mode goes to it as 16-bit mono at the rate the
 *    ISR really runs, with a zero for each period the queue was empty, and
 *    through the receiver's 75 us de-emphasis on the way (the master
 *    pre-emphasizes the stream, and a radio undoes it). So a host speaker
 *    plays what a radio would, and a wav backend records it on the dump's
 *    timeline. The dump itself keeps the wire's samples untouched.
 *
 * Everything the master does is written to the dump file, one line per
 * event, timestamped in virtual nanoseconds:
 *
 *   <ns> fm set-carrier|set-deviation|set-attenuation|set-mode <value>
 *   <ns> fm set-rate <hz> actual=<hz>
 *   <ns> fm audio count=<n> bits=<8|16> level=<queued> dropped=<n> data=<hex>
 *   <ns> fm underrun samples=<n>[ end]
 *   <ns> fm spi-overrun
 *   <ns> fm error byte=<hex> state=<n>
 *   <ns> fm get-status <hex> | get-level <n>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "system/system.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/chips/fm_transmitter.h"

/*
 * The slave's sample timer: TMR0 in 8-bit period mode clocked straight from
 * the 32 MHz HFINTOSC, with its output postscaler stretching the period.
 * A tick is 31.25 ns, so the sample timeline is kept in quarter-nanoseconds
 * (125 per tick) to stay exact.
 */
#define FM_TMR0_HZ 32000000u
#define FM_QNS_PER_TICK (4 * NANOSECONDS_PER_SECOND / FM_TMR0_HZ)
#define FM_SPI_OVERRUN_LOG_MAX 32

static void G_GNUC_PRINTF(2, 3)
fm_log(FMTransmitterState *s, const char *fmt, ...)
{
    va_list ap;

    if (!s->dump_file) {
        return;
    }
    fprintf(s->dump_file, "%" PRId64 " fm ",
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    va_start(ap, fmt);
    vfprintf(s->dump_file, fmt, ap);
    va_end(ap);
    fputc('\n', s->dump_file);
    fflush(s->dump_file);
}

static uint32_t fm_level(FMTransmitterState *s)
{
    return (s->tail - s->head) & (s->ring_slots - 1);
}

/*
 * The queue level lines, as the slave's sample ISR drives them: the band
 * steps up when the depth reaches a threshold, back down when it falls
 * the hysteresis below one, one band -- one Gray bit -- at a time. In
 * sine test the firmware's main loop keeps the ring topped up, which the
 * queue here does not model, so the band is pinned full there: a master
 * probing an unconfigured module sees band 3, exactly as it would the
 * boot tone's.
 */
static void fm_lvl_lines(FMTransmitterState *s)
{
    /* Gray: LVL0 high in bands 1 and 2, LVL1 in 2 and 3. */
    qemu_set_irq(s->lvl[0], s->lvl_band == 1 || s->lvl_band == 2);
    qemu_set_irq(s->lvl[1], s->lvl_band >= 2);
}

static void fm_lvl_update(FMTransmitterState *s)
{
    static const uint32_t rise[3] = { FM_LVL_T1, FM_LVL_T2, FM_LVL_T3 };
    uint32_t depth = s->mode == FM_MODE_SINE_TEST ? s->ring_slots - 1
                                                  : fm_level(s);

    while (s->lvl_band < 3 && depth >= rise[s->lvl_band]) {
        s->lvl_band++;
        fm_lvl_lines(s);
        fm_log(s, "level-band %u depth=%u", s->lvl_band, depth);
    }
    while (s->lvl_band > 0 && depth < rise[s->lvl_band - 1] - FM_LVL_HYST) {
        s->lvl_band--;
        fm_lvl_lines(s);
        fm_log(s, "level-band %u depth=%u", s->lvl_band, depth);
    }
}

/*
 * What the slave's TMR0 makes of each rate: (TMR0H + 1) * (T0OUTPS + 1)
 * ticks, the pair from the firmware's kSampleRates table (the closest to
 * 32 MHz / rate the postscaler can reach). Zero: not a supported rate.
 */
static uint32_t fm_period_ticks_of(uint32_t hz)
{
    switch (hz) {
    case 16000: return 250 * 8;   /* 16000.0 Hz */
    case 22050: return 242 * 6;   /* 22038.6 Hz */
    case 32000: return 250 * 4;   /* 32000.0 Hz */
    case 44100: return 242 * 3;   /* 44077.1 Hz */
    case 48000: return 222 * 3;   /* 48048.0 Hz */
    default:    return 0;
    }
}

static bool fm_rate_ok(uint32_t hz)
{
    return fm_period_ticks_of(hz) != 0;
}

/*
 * One sample period in quarter-nanoseconds, at the slave's oscillator:
 * nominal 32 MHz, stretched or shrunk by clock-ppm.
 */
/* The oscillator's error plus what the master has trimmed away. */
static int64_t fm_effective_ppm(FMTransmitterState *s)
{
    return (int64_t)s->clock_ppm + (int64_t)s->osctune * FM_OSCTUNE_STEP_PPM;
}

static int64_t fm_period_qns(FMTransmitterState *s)
{
    int64_t nominal = (int64_t)fm_period_ticks_of(s->sample_rate) *
                      FM_QNS_PER_TICK;

    return (nominal * 1000000 + 500000) / (1000000 + fm_effective_ppm(s));
}

/* Rounded to the nearest hertz, for the log and the audio backend. */
static uint32_t fm_actual_rate(FMTransmitterState *s)
{
    uint32_t ticks = fm_period_ticks_of(s->sample_rate);
    int64_t clock_hz_ppm = (int64_t)FM_TMR0_HZ * (1000000 + fm_effective_ppm(s));
    int64_t den = (int64_t)ticks * 1000000;

    return (uint32_t)((clock_hz_ppm + den / 2) / den);
}

/* `end`: the stream was stopped in this run, so it is the drain after the
   last samples rather than a gap in them. */
static void fm_end_underrun(FMTransmitterState *s, bool end)
{
    if (s->underrun_run) {
        fm_log(s, "underrun samples=%u%s", s->underrun_run, end ? " end" : "");
        s->underrun_run = 0;
    }
}

/*
 * The host audio side. In FM_MODE_FM_AUDIO the samples the ISR pops (and a
 * zero for every period it finds the queue empty, so the timeline of a
 * capture matches the dump's) are staged in out_buf; the backend takes them
 * from its callback at its own pace, which is how the mixing engine expects
 * to be fed -- pushing from the sample timer would lose whatever it had no
 * room for at that instant.
 */
static bool fm_playing(FMTransmitterState *s)
{
    return s->voice && s->mode == FM_MODE_FM_AUDIO;
}

/*
 * The receiver's side of the broadcast standard: the master pre-emphasizes
 * what it streams -- the 75 us US filter, y[n] = x[n] - round(alpha *
 * x[n-1]) with alpha = exp(-1/(rate * 75us)) in Q15 (FM_Preemph in the
 * controller's fm_master.h) -- and every radio de-emphasizes with the
 * matching low-pass. The host audio path stands in for the radio, so it
 * runs the exact inverse, x[n] = y[n] + round(alpha * x[n-1]), and what
 * the speaker or a wav capture gets is the movie's audio back. Only the
 * playback: the queue, the protocol and the dump keep the wire's samples.
 */
static int32_t fm_deemph_alpha_q15(uint32_t hz)
{
    switch (hz) {
    case 16000: return 14241;   /* exp(-1/1.20000) = 0.43460 */
    case 22050: return 17899;   /* exp(-1/1.65375) = 0.54625 */
    case 32000: return 21602;   /* exp(-1/2.40000) = 0.65924 */
    case 44100: return 24218;   /* exp(-1/3.30750) = 0.73908 */
    case 48000: return 24821;   /* exp(-1/3.60000) = 0.75747 */
    default:    return 0;
    }
}

static int16_t fm_deemph(FMTransmitterState *s, int16_t sample)
{
    int32_t x = sample + ((fm_deemph_alpha_q15(s->sample_rate) *
                           s->deemph_prev + (1 << 14)) >> 15);

    x = MIN(32767, MAX(-32768, x));
    s->deemph_prev = x;
    return (int16_t)x;
}

static void fm_out_push(FMTransmitterState *s, int16_t sample)
{
    uint32_t next = (s->out_tail + 1) & (FM_OUT_MAX - 1);

    if (next == s->out_head) {
        /* The host is not keeping up; the newest sample is the one lost. */
        return;
    }
    s->out_buf[s->out_tail] = sample;
    s->out_tail = next;
}

static void fm_audio_cb(void *opaque, int free_b)
{
    FMTransmitterState *s = opaque;

    while (free_b >= (int)sizeof(int16_t) && s->out_head != s->out_tail) {
        uint32_t avail = (s->out_tail - s->out_head) & (FM_OUT_MAX - 1);
        uint32_t contig = MIN(avail, FM_OUT_MAX - s->out_head);
        size_t bytes = MIN((size_t)contig * sizeof(int16_t),
                           (size_t)free_b & ~(sizeof(int16_t) - 1));
        size_t written = audio_be_write(s->audio_be, s->voice,
                                        &s->out_buf[s->out_head], bytes);

        if (written < sizeof(int16_t)) {
            break;
        }
        s->out_head = (s->out_head + written / sizeof(int16_t)) &
                      (FM_OUT_MAX - 1);
        free_b -= written;
    }
}

/*
 * Make the voice match the mode and rate: (re)opened at the rate the ISR
 * really runs (the host resamples from there), and running only while the
 * module is playing audio.
 */
static void fm_voice_sync(FMTransmitterState *s)
{
    struct audsettings as = {
        .freq = s->sample_rate ? fm_actual_rate(s) : 0,
        .nchannels = 1,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = false,
    };

    if (!s->audio_be || !s->sample_rate) {
        return;
    }
    s->voice = audio_be_open_out(s->audio_be, s->voice, "fm-transmitter",
                                 s, fm_audio_cb, &as);
    if (!fm_playing(s)) {
        s->out_head = s->out_tail = 0;
    }
    if (s->voice) {
        audio_be_set_active_out(s->audio_be, s->voice, fm_playing(s));
    }
}

static void fm_arm(FMTransmitterState *s)
{
    if (!timer_pending(s->tick)) {
        s->next_tick_qns = 4 * qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                           fm_period_qns(s);
        timer_mod(s->tick, s->next_tick_qns / 4);
    }
}

/* One sample period: the ISR pops a sample, or notes that there was none. */
static void fm_tick(void *opaque)
{
    FMTransmitterState *s = opaque;

    if (s->mode & 0x01) {
        if (s->head != s->tail) {
            int16_t sample = s->sample_buf[s->head];
            s->head = (s->head + 1) & (s->ring_slots - 1);
            fm_lvl_update(s);
            if (fm_playing(s)) {
                fm_out_push(s, fm_deemph(s, sample));
            }
        } else if (s->streaming) {
            s->status |= FM_STATUS_UNDERRUN;
            s->underrun_run++;
            if (fm_playing(s)) {
                fm_out_push(s, fm_deemph(s, 0));
            }
            if (s->underrun_run >= s->sample_rate) {
                /* A second of silence: the stream is over, say so once. */
                fm_end_underrun(s, true);
                s->streaming = false;
                return;
            }
        }
    }
    if (s->head != s->tail || s->streaming) {
        s->next_tick_qns += fm_period_qns(s);
        timer_mod(s->tick, s->next_tick_qns / 4);
    }
}

static bool fm_push(FMTransmitterState *s)
{
    uint32_t next = (s->tail + 1) & (s->ring_slots - 1);

    if (next == s->head) {
        return false;
    }
    fm_end_underrun(s, false);
    s->tail = next;
    fm_lvl_update(s);
    if (s->mode & 0x01) {
        fm_arm(s);
    }
    return true;
}

static void fm_log_packet(FMTransmitterState *s, bool partial)
{
    char hex[FM_AUDIO_PACKET_MAX * 4 + 1];
    unsigned i;

    for (i = 0; i < s->pkt_len; i++) {
        snprintf(hex + 2 * i, 3, "%02X", s->pkt[i]);
    }
    hex[2 * s->pkt_len] = 0;
    fm_log(s, "audio count=%u bits=%u level=%u dropped=%u%s data=%s",
           s->audio_len, s->audio_bits, s->pkt_level, s->pkt_dropped,
           partial ? " partial" : "", hex);
    s->pkt_len = 0;
}

/*
 * A sample has been assembled; queue it or report the drop. `next` is the
 * state the packet continues in -- the byte after this one is another
 * 8-bit sample, another 16-bit sample's high byte, or the next byte of a
 * 12-bit group, and only the caller knows which.
 */
static uint8_t fm_sample(FMTransmitterState *s, FMParserState next)
{
    if (fm_push(s)) {
        if (--s->audio_remain == 0) {
            s->state = FM_ST_IDLE;
            fm_log_packet(s, false);
            return FM_RESP_OK;
        }
        s->state = next;
        return FM_RESP_MORE;
    }
    s->pkt_dropped++;
    if (--s->audio_remain == 0) {
        s->state = FM_ST_IDLE;
    } else {
        s->state = FM_ST_DROP_ONE;
    }
    fm_log_packet(s, s->audio_remain != 0);
    return FM_RESP_OVERFLOW;
}

static uint8_t fm_error(FMTransmitterState *s, uint8_t b)
{
    fm_log(s, "error byte=0x%02X state=%u", b, s->state);
    s->state = FM_ST_IDLE;
    return FM_RESP_ERROR;
}

/*
 * Version 2 replies are synchronous -- the Hz-to-register math moved to
 * the master, and the slave's remaining hardware work (the PLL write, the
 * table rebuild) runs in main-loop slices that never starve its SPI
 * service. What the mock models of that is only its visibility: while the
 * slices run, FM_CMD_NOP answers FM_RESP_APPLYING. The windows are the
 * slice work's rough cost on the 8 MIPS core.
 */
#define FM_APPLY_CARRIER_NS     60000
#define FM_APPLY_DEVIATION_NS  250000

static uint8_t fm_apply(FMTransmitterState *s, int64_t cost_ns)
{
    s->applying_until_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + cost_ns;
    return FM_RESP_OK;
}

static uint8_t fm_process(FMTransmitterState *s, uint8_t b)
{
    switch (s->state) {
    case FM_ST_AUDIO_DATA:
        if (s->pkt_len < sizeof(s->pkt)) {
            s->pkt[s->pkt_len++] = b;
        }
        s->sample_buf[s->tail] = ((int16_t)(int8_t)b) << 8;
        return fm_sample(s, FM_ST_AUDIO_DATA);

    /*
     * 12-bit: three bytes carry two samples, nibbles most significant
     * first. The slave scales a sample's three nibbles with the same
     * tables it uses for the top three of a 16-bit one, so a 12-bit
     * sample is exactly that sample left-justified into sixteen.
     */
    case FM_ST_AUDIO_12_HI:
        if (s->pkt_len < sizeof(s->pkt)) {
            s->pkt[s->pkt_len++] = b;
        }
        s->audio_hi = b;
        s->state = FM_ST_AUDIO_12_MID;
        return FM_RESP_MORE;

    case FM_ST_AUDIO_12_LO:
    case FM_ST_AUDIO_12_MID: {
        bool mid = s->state == FM_ST_AUDIO_12_MID;
        uint16_t v;

        if (s->pkt_len < sizeof(s->pkt)) {
            s->pkt[s->pkt_len++] = b;
        }
        if (mid) {
            v = (uint16_t)(((uint16_t)s->audio_hi << 8) | (b & 0xF0));
            s->audio_hi = b;
        } else {
            v = (uint16_t)(((uint16_t)(s->audio_hi & 0x0F) << 12) |
                           ((uint16_t)b << 4));
        }
        s->sample_buf[s->tail] = (int16_t)v;
        return fm_sample(s, mid ? FM_ST_AUDIO_12_LO : FM_ST_AUDIO_12_HI);
    }

    case FM_ST_IDLE:
        s->accum = 0;
        switch (b) {
        case FM_CMD_NOP:
            if (s->applying_until_ns &&
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->applying_until_ns) {
                return FM_RESP_APPLYING;
            }
            s->applying_until_ns = 0;
            return (s->status & (FM_STATUS_ISR_OVERRUN |
                                 FM_STATUS_SPI_OVERRUN)) ?
                   FM_RESP_FAULT : FM_RESP_READY;
        case FM_CMD_GET_STATUS: {
            uint8_t st = s->status;

            fm_log(s, "get-status 0x%02X", st);
            s->status = 0;
            return st;
        }
        case FM_CMD_GET_LEVEL: {
            uint32_t level = fm_level(s);

            fm_log(s, "get-level %u", level);
            return level > 255 ? 255 : level;
        }
        case FM_CMD_SET_CARRIER:
            s->state = FM_ST_CARRIER;
            s->accum_left = 5;
            return FM_RESP_MORE;
        case FM_CMD_SET_DEVIATION:
            s->state = FM_ST_DEVIATION;
            s->accum_left = 2;
            return FM_RESP_MORE;
        case FM_CMD_SET_ATTENUATION:
            s->state = FM_ST_ATTENUATION;
            return FM_RESP_MORE;
        case FM_CMD_SET_OSCTUNE:
            s->state = FM_ST_OSCTUNE;
            return FM_RESP_MORE;
        case FM_CMD_SET_SAMPLE_RATE:
            s->state = FM_ST_RATE;
            s->accum_left = 3;
            return FM_RESP_MORE;
        case FM_CMD_SET_MODE:
            s->state = FM_ST_MODE;
            return FM_RESP_MORE;
        case FM_CMD_AUDIO_DATA:
            s->state = FM_ST_AUDIO_BITS;
            return FM_RESP_MORE;
        default:
            return fm_error(s, b);
        }

    case FM_ST_CARRIER: {
        uint16_t n;
        uint32_t inc;

        if (--s->accum_left) {
            s->accum = (s->accum << 8) | b;
            return FM_RESP_MORE;
        }
        s->state = FM_ST_IDLE;
        /* N (16 bits) then the NCO increment (24), both precomputed by
           the master and too wide together for the accumulator, so the
           last byte rides separately -- as in the firmware's parser. The
           mock validates the same ranges and reconstructs the RF for the
           dump (exact wherever the increment is: the 512/15625 map is
           invertible there). */
        n = (s->accum >> 16) & 0xFFFF;
        inc = ((s->accum & 0xFFFF) << 8) | b;
        if (n < FM_CARRIER_N_MIN || n > FM_CARRIER_N_MAX ||
            inc < FM_NCO_IF_INC_MIN || inc > FM_NCO_IF_INC_MAX) {
            return fm_error(s, b);
        }
        s->carrier_hz = (uint32_t)(n * 100000u -
                                   (uint64_t)inc * 15625 / 512);
        fm_log(s, "set-carrier %u n=%u inc=%u", s->carrier_hz, n, inc);
        return fm_apply(s, FM_APPLY_CARRIER_NS);
    }

    case FM_ST_DEVIATION:
        s->accum = (s->accum << 8) | b;
        if (--s->accum_left) {
            return FM_RESP_MORE;
        }
        s->state = FM_ST_IDLE;
        /* The increment scale k, precomputed by the master. */
        if (s->accum > FM_DEV_SCALE_MAX) {
            return fm_error(s, b);
        }
        s->deviation_k = s->accum;
        fm_log(s, "set-deviation %u", s->deviation_k);
        return fm_apply(s, FM_APPLY_DEVIATION_NS);

    case FM_ST_ATTENUATION:
        s->state = FM_ST_IDLE;
        s->attenuation_db = b & 0x1F;
        fm_log(s, "set-attenuation %u", s->attenuation_db);
        return FM_RESP_OK;

    case FM_ST_OSCTUNE: {
        int8_t tune = (int8_t)b;

        s->state = FM_ST_IDLE;
        if (tune < -32) {
            tune = -32;
        }
        if (tune > 31) {
            tune = 31;
        }
        s->osctune = tune;
        fm_voice_sync(s);
        fm_log(s, "set-osctune %d effective-ppm=%d", tune,
               (int)fm_effective_ppm(s));
        /* The tune moved the sample clock: restate the actual rate, so a
           dump reader's rate_actual tracks it without a new set-rate. */
        if (s->sample_rate) {
            fm_log(s, "set-rate %u actual=%u", s->sample_rate,
                   fm_actual_rate(s));
        }
        return FM_RESP_OK;
    }

    case FM_ST_RATE:
        s->accum = (s->accum << 8) | b;
        if (--s->accum_left) {
            return FM_RESP_MORE;
        }
        s->state = FM_ST_IDLE;
        if (!fm_rate_ok(s->accum)) {
            return fm_error(s, b);
        }
        s->sample_rate = s->accum;
        fm_voice_sync(s);
        fm_log(s, "set-rate %u actual=%u", s->sample_rate,
               fm_actual_rate(s));
        return FM_RESP_OK;

    case FM_ST_MODE:
        s->state = FM_ST_IDLE;
        if (b > FM_MODE_SINE_TEST) {
            return fm_error(s, b);
        }
        fm_end_underrun(s, true);
        /* Leaving a modulating mode purges the queue, as the firmware
           does: leftovers are stranded when modulation stops, and the
           master's level estimator seeds on "a configured stream starts
           empty". Entering one keeps the queue -- preloading samples in
           SILENCE and starting them with the mode switch still works. */
        if (s->mode & 0x01) {
            s->head = s->tail;
        }
        s->mode = b;
        s->streaming = false;
        /* A stream starts afresh, and so does the master's pre-emphasis
           state; the receiver's inverse starts with it. */
        s->deemph_prev = 0;
        fm_voice_sync(s);
        fm_log(s, "set-mode %u", s->mode);
        fm_lvl_update(s);
        if ((s->mode & 0x01) && s->head != s->tail) {
            fm_arm(s);
        }
        return FM_RESP_OK;

    case FM_ST_AUDIO_BITS:
        if ((b == FM_AUDIO_BITS_8 &&
             s->sample_rate <= FM_SAMPLE_RATE_MAX_8BIT_HZ) ||
            (b == FM_AUDIO_BITS_12 &&
             s->sample_rate <= FM_SAMPLE_RATE_MAX_12BIT_HZ)) {
            s->audio_bits = b;
            s->state = FM_ST_AUDIO_LEN;
            if (s->mode == FM_MODE_FM_AUDIO) {
                s->streaming = true;
                fm_arm(s);
            }
            return FM_RESP_MORE;
        }
        return fm_error(s, b);

    case FM_ST_AUDIO_LEN:
        if (b == 0 || b > FM_AUDIO_PACKET_MAX ||
            (s->audio_bits == FM_AUDIO_BITS_12 && (b & 1))) {
            /* An odd 12-bit length would end the packet halfway through
               a group, with half a sample's nibbles unplaced. */
            return fm_error(s, b);
        }
        s->audio_len = b;
        s->audio_remain = b;
        s->pkt_len = 0;
        s->pkt_dropped = 0;
        s->pkt_level = fm_level(s);
        s->state = s->audio_bits == FM_AUDIO_BITS_12 ? FM_ST_AUDIO_12_HI
                                                     : FM_ST_AUDIO_DATA;
        return FM_RESP_MORE;

    case FM_ST_DROP_ONE:
        /* Sent while OVERFLOW was in flight; not a command. */
        s->state = FM_ST_IDLE;
        return FM_RESP_READY;

    default:
        return fm_error(s, b);
    }
}

static uint32_t fm_transmitter_transfer(SSIPeripheral *dev, uint32_t val)
{
    FMTransmitterState *s = FM_TRANSMITTER(dev);
    uint8_t b = val & 0xFF;
    uint8_t out;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    out = s->resp;

    /*
     * The slave reads its receive register from the main loop, so a byte
     * that lands before the previous one was taken is lost -- SSPOV on the
     * PIC16 -- and the response the master reads is the stale one.
     */
    if (now < s->busy_until_ns) {
        s->status |= FM_STATUS_SPI_OVERRUN;
        if (++s->spi_overruns <= FM_SPI_OVERRUN_LOG_MAX) {
            fm_log(s, "spi-overrun%s",
                   s->spi_overruns == FM_SPI_OVERRUN_LOG_MAX ?
                   " (further ones not logged)" : "");
        }
        return out;
    }
    s->busy_until_ns = now + s->byte_cost_ns;
    s->resp = fm_process(s, b);
    return out;
}

static int fm_transmitter_set_cs(SSIPeripheral *dev, bool cs)
{
    FMTransmitterState *s = FM_TRANSMITTER(dev);

    /* The line high is the chip deselected. */
    if (cs && s->selected) {
        if (s->state == FM_ST_AUDIO_DATA ||
            s->state == FM_ST_AUDIO_12_HI ||
            s->state == FM_ST_AUDIO_12_MID ||
            s->state == FM_ST_AUDIO_12_LO) {
            fm_log_packet(s, true);
        }
        /*
         * Version 2 framing: the select rising ends the transaction and
         * resets the parser -- the firmware does it with an
         * interrupt-on-change on its SS pin (RC6). A command cut short
         * costs exactly that transaction.
         */
        s->state = FM_ST_IDLE;
        s->accum = 0;
        s->accum_left = 0;
        s->audio_remain = 0;
        s->resp = FM_RESP_READY;
    }
    s->selected = !cs;
    return 0;
}

/*
 * At exit. Devices are not finalized on the way out, and a voice left open
 * holds its backend open, so a wav backend would never write its lengths
 * into the header: close the voice here and the backend can finish.
 */
static void fm_transmitter_exit(Notifier *n, void *data)
{
    FMTransmitterState *s = container_of(n, FMTransmitterState, exit);

    if (s->voice) {
        audio_be_close_out(s->audio_be, s->voice);
        s->voice = NULL;
    }
}

static void fm_transmitter_realize(SSIPeripheral *dev, Error **errp)
{
    FMTransmitterState *s = FM_TRANSMITTER(dev);

    if (s->ring_slots < 2 || s->ring_slots > FM_RING_MAX ||
        (s->ring_slots & (s->ring_slots - 1))) {
        error_setg(errp, "fm-transmitter: ring-slots must be a power of two "
                   "between 2 and %d", FM_RING_MAX);
        return;
    }
    if (s->clock_ppm < -100000 || s->clock_ppm > 100000) {
        error_setg(errp, "fm-transmitter: clock-ppm must be within +/-100000 "
                   "(10%%)");
        return;
    }
    if (s->dump_path) {
        s->dump_file = fopen(s->dump_path, "w");
        if (!s->dump_file) {
            error_setg_errno(errp, errno, "failed to open audio dump file '%s'",
                             s->dump_path);
            return;
        }
    }
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, fm_tick, s);
    qdev_init_gpio_out_named(DEVICE(s), s->lvl, FM_LVL_LINES_GPIO, 2);
    /*
     * The voice is opened at reset, once the rate is known. There is no
     * fallback to the default audiodev: the module is silent unless the
     * board names a backend for it, so a check run never reaches a speaker.
     */
    if (s->audio_be) {
        s->exit.notify = fm_transmitter_exit;
        qemu_add_exit_notifier(&s->exit);
    }
}

static void fm_transmitter_reset_hold(Object *obj, ResetType type)
{
    FMTransmitterState *s = FM_TRANSMITTER(obj);

    if (s->tick) {
        timer_del(s->tick);
    }
    s->carrier_hz = 98000000;
    s->deviation_k = 2457;
    s->sample_rate = 22050;   /* the slave's reset default: both depths stream at it */
    s->attenuation_db = 0;
    s->osctune = 0;
    s->mode = FM_MODE_SINE_TEST;
    s->status = 0;
    s->state = FM_ST_IDLE;
    s->resp = FM_RESP_READY;
    s->accum = 0;
    s->accum_left = 0;
    s->audio_bits = FM_AUDIO_BITS_8;
    s->audio_len = 0;
    s->audio_remain = 0;
    s->audio_hi = 0;
    s->streaming = false;
    s->head = 0;
    s->tail = 0;
    s->next_tick_qns = 0;
    s->busy_until_ns = 0;
    s->applying_until_ns = 0;
    s->selected = false;
    s->pkt_len = 0;
    s->pkt_dropped = 0;
    s->pkt_level = 0;
    s->underrun_run = 0;
    s->spi_overruns = 0;
    s->lvl_band = 0;
    fm_lvl_lines(s);
    fm_lvl_update(s);   /* sine test at power-on: the lines walk to band 3 */
    fm_voice_sync(s);
}

static int fm_transmitter_post_load(void *opaque, int version_id)
{
    FMTransmitterState *s = opaque;

    fm_voice_sync(s);
    return 0;
}

static const Property fm_transmitter_properties[] = {
    DEFINE_PROP_STRING("dump", FMTransmitterState, dump_path),
    DEFINE_AUDIO_PROPERTIES(FMTransmitterState, audio_be),
    /* FM_AUDIO_BUF_SIZE in the slave firmware; one slot is the sentinel. */
    DEFINE_PROP_UINT32("ring-slots", FMTransmitterState, ring_slots, 256),
    /*
     * The slave's main loop takes ~78 instructions at 8 MIPS for an 8-bit
     * sample byte and ~172 for a 16-bit pair (PROTOCOL.md, "Timing
     * budget"); a byte closer than this to the previous one is lost.
     */
    DEFINE_PROP_UINT32("byte-cost-ns", FMTransmitterState, byte_cost_ns,
                       10750),
    /*
     * The slave's oscillator error: its HFINTOSC is good to a percent or
     * two, and both its sample clock and its carrier follow it. Positive
     * runs the sample timer fast (the queue drains sooner than the
     * master's nominal rate says).
     */
    DEFINE_PROP_INT32("clock-ppm", FMTransmitterState, clock_ppm, 0),
};

static const VMStateDescription fm_transmitter_vmstate = {
    .name = "fm-transmitter",
    .version_id = 6,
    .minimum_version_id = 6,
    .post_load = fm_transmitter_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, FMTransmitterState),
        VMSTATE_INT16_ARRAY(sample_buf, FMTransmitterState, FM_RING_MAX),
        VMSTATE_UINT32(carrier_hz, FMTransmitterState),
        VMSTATE_UINT16(deviation_k, FMTransmitterState),
        VMSTATE_UINT32(sample_rate, FMTransmitterState),
        VMSTATE_UINT8(attenuation_db, FMTransmitterState),
        VMSTATE_INT8(osctune, FMTransmitterState),
        VMSTATE_UINT8(mode, FMTransmitterState),
        VMSTATE_UINT8(status, FMTransmitterState),
        VMSTATE_UINT8(state, FMTransmitterState),
        VMSTATE_UINT8(resp, FMTransmitterState),
        VMSTATE_UINT32(accum, FMTransmitterState),
        VMSTATE_UINT8(accum_left, FMTransmitterState),
        VMSTATE_UINT8(audio_bits, FMTransmitterState),
        VMSTATE_UINT8(audio_len, FMTransmitterState),
        VMSTATE_UINT8(audio_remain, FMTransmitterState),
        VMSTATE_UINT8(audio_hi, FMTransmitterState),
        VMSTATE_BOOL(streaming, FMTransmitterState),
        VMSTATE_UINT32(head, FMTransmitterState),
        VMSTATE_UINT32(tail, FMTransmitterState),
        VMSTATE_UINT8(lvl_band, FMTransmitterState),
        VMSTATE_TIMER_PTR(tick, FMTransmitterState),
        VMSTATE_INT64(next_tick_qns, FMTransmitterState),
        VMSTATE_INT64(busy_until_ns, FMTransmitterState),
        VMSTATE_BOOL(selected, FMTransmitterState),
        VMSTATE_END_OF_LIST()
    }
};

static void fm_transmitter_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->realize = fm_transmitter_realize;
    k->transfer = fm_transmitter_transfer;
    k->set_cs = fm_transmitter_set_cs;
    k->cs_polarity = SSI_CS_LOW;
    device_class_set_props(dc, fm_transmitter_properties);
    dc->vmsd = &fm_transmitter_vmstate;
    rc->phases.hold = fm_transmitter_reset_hold;
}

static const TypeInfo fm_transmitter_info = {
    .name          = TYPE_FM_TRANSMITTER,
    .parent        = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(FMTransmitterState),
    .class_init    = fm_transmitter_class_init,
};

static void fm_transmitter_register_types(void)
{
    type_register_static(&fm_transmitter_info);
}

type_init(fm_transmitter_register_types)
