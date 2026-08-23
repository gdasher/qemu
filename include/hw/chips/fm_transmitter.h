/*
 * FM Radio Transmitter SPI Controller (XMASNGFMv2)
 *
 * The SPI slave protocol of the FM transmitter module, as PROTOCOL.md in
 * the XMASNGFMv2 repository describes it, with the timing that matters to a
 * master feeding it: a sample queue drained at the sample rate, and a
 * receive path that takes a fixed time per byte.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHIPS_FM_TRANSMITTER_H
#define HW_CHIPS_FM_TRANSMITTER_H

#include "hw/ssi/ssi.h"
#include "qemu/audio.h"
#include "qemu/notify.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_FM_TRANSMITTER "fm-transmitter"
OBJECT_DECLARE_SIMPLE_TYPE(FMTransmitterState, FM_TRANSMITTER)

/* Commands */
#define FM_CMD_NOP              0x00
#define FM_CMD_SET_CARRIER      0x01
#define FM_CMD_SET_DEVIATION    0x02
#define FM_CMD_SET_ATTENUATION  0x03
#define FM_CMD_SET_SAMPLE_RATE  0x04
#define FM_CMD_SET_MODE         0x05
#define FM_CMD_AUDIO_DATA       0x06
#define FM_CMD_GET_STATUS       0x07
#define FM_CMD_GET_LEVEL        0x08
#define FM_CMD_SET_OSCTUNE      0x09

/* One OSCTUNE step moves the slave's HFINTOSC by about this much. */
#define FM_OSCTUNE_STEP_PPM     1000

/* Responses (protocol version 2: replies are synchronous, ERROR moved
   off 0xFF, BUSY retired, APPLYING reports staged slice work on NOP) */
#define FM_RESP_READY           0x00
#define FM_RESP_MORE            0x01
#define FM_RESP_OK              0x02
#define FM_RESP_OVERFLOW        0x03
#define FM_RESP_FAULT           0x04
#define FM_RESP_APPLYING        0x06
#define FM_RESP_ERROR           0x0E
#define FM_RESP_EMPTY           0xFF

/* Status bits */
#define FM_STATUS_ISR_OVERRUN   0x01
#define FM_STATUS_SPI_OVERRUN   0x02
#define FM_STATUS_UNDERRUN      0x04

/* Modes */
#define FM_MODE_SILENCE         0x00
#define FM_MODE_FM_AUDIO        0x01
#define FM_MODE_CW              0x02
#define FM_MODE_SINE_TEST       0x03

#define FM_AUDIO_BITS_8         8
#define FM_AUDIO_BITS_12        12
#define FM_AUDIO_BITS_16        16      /* retired; the slave refuses it */
#define FM_AUDIO_PACKET_MAX     64

#define FM_CARRIER_MIN_HZ       76000000u
#define FM_CARRIER_MAX_HZ       108000000u
#define FM_DEVIATION_MAX_HZ     200000u

/* SET_CARRIER's precomputed form: the ADF4002 N divider and the NCO base
   increment; SET_DEVIATION's: the increment scale k (PROTOCOL.md v2). */
#define FM_CARRIER_N_MIN        860
#define FM_CARRIER_N_MAX        1180
#define FM_NCO_IF_INC_MIN       324403
#define FM_NCO_IF_INC_MAX       327680
#define FM_DEV_SCALE_MAX        6553
#define FM_SAMPLE_RATE_MAX_HZ   48000u
/*
 * The per-depth ceilings the slave enforces: not what its sample interrupt
 * can play out, but what its receive path can take off the wire inside a
 * frame (XMASNGFMv2 OPTIMIZATION.md section 3). Mirrors
 * FM_SAMPLE_RATE_MAX_*_HZ in its fm_radio_interface.h.
 */
#define FM_SAMPLE_RATE_MAX_8BIT_HZ  32000u
#define FM_SAMPLE_RATE_MAX_12BIT_HZ 22050u

/*
 * The queue level lines: a Gray-coded band of the queue depth on two
 * GPIOs (the slave's RC0/RC7), stepped one band -- one line -- at a
 * time with hysteresis. Mirrors FM_LVL_* in the slave firmware's
 * fm_radio_interface.h and the master's fm_master.h.
 */
#define FM_LVL_LINES_GPIO "lvl"
#define FM_LVL_T1        32
#define FM_LVL_T2        96
#define FM_LVL_T3        248
#define FM_LVL_HYST      8

/* The largest queue the model can be given (a power of two). */
#define FM_RING_MAX 1024

/* Samples popped by the ISR and not yet taken by the host audio backend. */
#define FM_OUT_MAX 8192

typedef enum {
    FM_ST_IDLE,
    FM_ST_CARRIER,
    FM_ST_DEVIATION,
    FM_ST_ATTENUATION,
    FM_ST_OSCTUNE,
    FM_ST_RATE,
    FM_ST_MODE,
    FM_ST_AUDIO_BITS,
    FM_ST_AUDIO_LEN,
    FM_ST_AUDIO_DATA,
    /* 12-bit: two samples to three bytes, six nibbles most significant
       first (the slave's kState12Hi/Mid/Lo). */
    FM_ST_AUDIO_12_HI,
    FM_ST_AUDIO_12_MID,
    FM_ST_AUDIO_12_LO,
    FM_ST_DROP_ONE,
} FMParserState;

typedef struct FMDriftProfile {
    bool    enabled;
    int32_t thermal_max_ppm;   /* e.g. +3500 ppm */
    int32_t thermal_tau_ms;    /* e.g. 2000 ms time constant */
    int32_t linear_ppm_per_s;  /* e.g. +50 ppm/s */
    int32_t ripple_amp_ppm;    /* e.g. 100 ppm */
    double  ripple_freq_hz;    /* e.g. 2.0 Hz */
    int32_t jitter_sigma_ppm;  /* e.g. 10 ppm */
} FMDriftProfile;

struct FMTransmitterState {
    SSIPeripheral parent_obj;

    /*
     * Where the demodulated audio goes: the host backend named by the
     * "audiodev" property, if any. Samples the ISR pops wait in out_buf for
     * the backend's callback, which takes them at the backend's pace.
     */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    Notifier exit;              /* closes the voice so the backend can finish */
    int16_t out_buf[FM_OUT_MAX];
    uint32_t out_head;
    uint32_t out_tail;
    /*
     * The receiver's 75 us de-emphasis (the audio path plays what a radio
     * would, and every US broadcast radio de-emphasizes): the previous
     * output sample. Playback state only -- the queue, the dump and the
     * protocol carry the wire's pre-emphasized samples untouched.
     */
    int32_t deemph_prev;

    char *dump_path;
    FILE *dump_file;
    char *drift_profile_str;
    FMDriftProfile drift;
    uint32_t ring_slots;        /* queue entries; one is the sentinel */
    uint32_t byte_cost_ns;      /* how long the slave holds a received byte */
    int32_t clock_ppm;          /* the slave's oscillator error, parts per million */

    int16_t sample_buf[FM_RING_MAX];

    /* Configuration the master has applied. */
    uint32_t carrier_hz;        /* reconstructed from the N/INC pair */
    uint16_t deviation_k;       /* the increment scale, as sent */
    uint32_t sample_rate;
    uint8_t attenuation_db;
    int8_t osctune;             /* HFTUN, applied on top of clock-ppm */
    uint8_t mode;
    uint8_t status;

    /* Parser. */
    uint8_t state;
    uint8_t resp;               /* what the next exchange clocks out */
    uint32_t accum;
    uint8_t accum_left;
    uint8_t audio_bits;
    uint8_t audio_len;
    uint8_t audio_remain;
    uint8_t audio_hi;
    bool streaming;

    /* The sample queue and its clock. */
    uint32_t head;
    uint32_t tail;
    uint8_t lvl_band;           /* what the level lines show */
    qemu_irq lvl[2];            /* LVL0, LVL1 */
    QEMUTimer *tick;
    int64_t next_tick_qns;   /* quarter-nanoseconds: a TMR0 tick is 31.25 ns */
    int64_t busy_until_ns;
    /*
     * Staged slice work (the PLL write, the table rebuild) is running
     * until this moment: FM_CMD_NOP answers FM_RESP_APPLYING while it
     * is. The parser keeps taking bytes throughout -- version 2's whole
     * point. Zero when nothing is staged.
     */
    int64_t applying_until_ns;
    bool selected;

    /* The packet in flight, for the log. */
    uint8_t pkt[FM_AUDIO_PACKET_MAX * 2];
    uint16_t pkt_len;
    uint16_t pkt_dropped;
    uint32_t pkt_level;
    uint32_t underrun_run;
    uint32_t spi_overruns;
};

#endif
