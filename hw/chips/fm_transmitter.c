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
 *    ISR really runs, with a zero for each period the queue was empty. So a
 *    host speaker plays what a radio would, and a wav backend records it on
 *    the dump's timeline.
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
static int64_t fm_period_qns(FMTransmitterState *s)
{
    int64_t nominal = (int64_t)fm_period_ticks_of(s->sample_rate) *
                      FM_QNS_PER_TICK;

    return (nominal * 1000000 + 500000) / (1000000 + s->clock_ppm);
}

/* Rounded to the nearest hertz, for the log and the audio backend. */
static uint32_t fm_actual_rate(FMTransmitterState *s)
{
    uint32_t ticks = fm_period_ticks_of(s->sample_rate);
    int64_t clock_hz_ppm = (int64_t)FM_TMR0_HZ * (1000000 + s->clock_ppm);
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
            if (fm_playing(s)) {
                fm_out_push(s, sample);
            }
        } else if (s->streaming) {
            s->status |= FM_STATUS_UNDERRUN;
            s->underrun_run++;
            if (fm_playing(s)) {
                fm_out_push(s, 0);
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

/* A sample has been assembled; queue it or report the drop. */
static uint8_t fm_sample(FMTransmitterState *s)
{
    if (fm_push(s)) {
        if (--s->audio_remain == 0) {
            s->state = FM_ST_IDLE;
            fm_log_packet(s, false);
            return FM_RESP_OK;
        }
        s->state = FM_ST_AUDIO_DATA;
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
 * What a setting costs the slave to apply, on its 8 MIPS core. These are not
 * the price of a byte -- they are the work a command's last byte sets off,
 * and it is far longer than the gap between bytes at any wire rate chosen to
 * match an audio stream. So the slave answers FM_RESP_BUSY, does the work,
 * and has the result waiting; a master that keeps clocking meanwhile reads
 * BUSY, and a master that does not wait at all never sees the result.
 *
 * Measured against the firmware (XMASNGFMv2 src/fm_radio_interface.c) in the
 * pic16-devboard machine at its real speed: the carrier is a pair of 32-bit
 * divisions and a 24-bit latch write to the PLL; the deviation rebuilds four
 * scaling tables. The rest are register writes and cost almost nothing, but
 * they answer BUSY too, because the protocol is easier to get right when the
 * answer does not depend on how expensive the command happened to be.
 */
#define FM_APPLY_CARRIER_NS    150000
#define FM_APPLY_DEVIATION_NS  130000
#define FM_APPLY_RATE_NS        20000
#define FM_APPLY_MODE_NS        20000
#define FM_APPLY_ATTEN_NS       10000

static uint8_t fm_apply(FMTransmitterState *s, int64_t cost_ns)
{
    s->apply_until_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + cost_ns;
    s->apply_resp = FM_RESP_OK;
    return FM_RESP_BUSY;
}

static uint8_t fm_process(FMTransmitterState *s, uint8_t b)
{
    switch (s->state) {
    case FM_ST_AUDIO_DATA:
        if (s->pkt_len < sizeof(s->pkt)) {
            s->pkt[s->pkt_len++] = b;
        }
        if (s->audio_bits == FM_AUDIO_BITS_8) {
            s->sample_buf[s->tail] = ((int16_t)(int8_t)b) << 8;
            return fm_sample(s);
        }
        s->audio_hi = b;
        s->state = FM_ST_AUDIO_LO;
        return FM_RESP_MORE;

    case FM_ST_AUDIO_LO:
        if (s->pkt_len < sizeof(s->pkt)) {
            s->pkt[s->pkt_len++] = b;
        }
        s->sample_buf[s->tail] = (int16_t)(((uint16_t)s->audio_hi << 8) | b);
        return fm_sample(s);

    case FM_ST_IDLE:
        s->accum = 0;
        switch (b) {
        case FM_CMD_NOP:
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
            s->accum_left = 4;
            return FM_RESP_MORE;
        case FM_CMD_SET_DEVIATION:
            s->state = FM_ST_DEVIATION;
            s->accum_left = 3;
            return FM_RESP_MORE;
        case FM_CMD_SET_ATTENUATION:
            s->state = FM_ST_ATTENUATION;
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

    case FM_ST_CARRIER:
        s->accum = (s->accum << 8) | b;
        if (--s->accum_left) {
            return FM_RESP_MORE;
        }
        s->state = FM_ST_IDLE;
        if (s->accum < FM_CARRIER_MIN_HZ || s->accum > FM_CARRIER_MAX_HZ) {
            return fm_error(s, b);
        }
        s->carrier_hz = s->accum;
        fm_log(s, "set-carrier %u", s->carrier_hz);
        return fm_apply(s, FM_APPLY_CARRIER_NS);

    case FM_ST_DEVIATION:
        s->accum = (s->accum << 8) | b;
        if (--s->accum_left) {
            return FM_RESP_MORE;
        }
        s->state = FM_ST_IDLE;
        if (s->accum > FM_DEVIATION_MAX_HZ) {
            return fm_error(s, b);
        }
        s->deviation_hz = s->accum;
        fm_log(s, "set-deviation %u", s->deviation_hz);
        return fm_apply(s, FM_APPLY_DEVIATION_NS);

    case FM_ST_ATTENUATION:
        s->state = FM_ST_IDLE;
        s->attenuation_db = b & 0x1F;
        fm_log(s, "set-attenuation %u", s->attenuation_db);
        return fm_apply(s, FM_APPLY_ATTEN_NS);

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
        return fm_apply(s, FM_APPLY_RATE_NS);

    case FM_ST_MODE:
        s->state = FM_ST_IDLE;
        if (b > FM_MODE_SINE_TEST) {
            return fm_error(s, b);
        }
        fm_end_underrun(s, true);
        if (s->mode == FM_MODE_SINE_TEST) {
            s->head = s->tail;
        }
        s->mode = b;
        s->streaming = false;
        fm_voice_sync(s);
        fm_log(s, "set-mode %u", s->mode);
        if ((s->mode & 0x01) && s->head != s->tail) {
            fm_arm(s);
        }
        return fm_apply(s, FM_APPLY_MODE_NS);

    case FM_ST_AUDIO_BITS:
        if (b == FM_AUDIO_BITS_8 ||
            (b == FM_AUDIO_BITS_16 &&
             s->sample_rate <= FM_SAMPLE_RATE_MAX_16BIT_HZ)) {
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
        if (b == 0 || b > FM_AUDIO_PACKET_MAX) {
            return fm_error(s, b);
        }
        s->audio_len = b;
        s->audio_remain = b;
        s->pkt_len = 0;
        s->pkt_dropped = 0;
        s->pkt_level = fm_level(s);
        s->state = FM_ST_AUDIO_DATA;
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

    /*
     * A setting is being applied. The slave's transmit register holds BUSY
     * for the whole of it and its receive register is not being read, so a
     * master that keeps clocking reads BUSY and loses the bytes it sent --
     * which is what the protocol tells it to do, so those losses are polls
     * rather than the overrun below.
     */
    if (s->apply_until_ns) {
        if (now < s->apply_until_ns) {
            return FM_RESP_BUSY;
        }
        s->apply_until_ns = 0;
        s->resp = s->apply_resp;
    }
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
        if (s->state == FM_ST_AUDIO_DATA || s->state == FM_ST_AUDIO_LO) {
            fm_log_packet(s, true);
        }
        /*
         * The slave's parser does not watch SS: a command cut short by
         * deselection resumes where it was on the next transaction. The
         * response byte the slave pre-loaded stays where it is too.
         */
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
    s->deviation_hz = 75000;
    s->sample_rate = 44100;
    s->attenuation_db = 0;
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
    s->apply_until_ns = 0;
    s->apply_resp = FM_RESP_READY;
    s->selected = false;
    s->pkt_len = 0;
    s->pkt_dropped = 0;
    s->pkt_level = 0;
    s->underrun_run = 0;
    s->spi_overruns = 0;
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
    .version_id = 4,
    .minimum_version_id = 4,
    .post_load = fm_transmitter_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, FMTransmitterState),
        VMSTATE_INT16_ARRAY(sample_buf, FMTransmitterState, FM_RING_MAX),
        VMSTATE_UINT32(carrier_hz, FMTransmitterState),
        VMSTATE_UINT32(deviation_hz, FMTransmitterState),
        VMSTATE_UINT32(sample_rate, FMTransmitterState),
        VMSTATE_UINT8(attenuation_db, FMTransmitterState),
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
