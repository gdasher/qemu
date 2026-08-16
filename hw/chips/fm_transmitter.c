/*
 * FM Radio Transmitter SPI Controller (XMASNGFMv2)
 *
 * Emulates the SPI command interface and audio packet stream receiver,
 * logging configuration and audio data traffic with virtual time timestamps.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "hw/chips/fm_transmitter.h"

static void fm_transmitter_log_audio_packet(FMTransmitterState *s)
{
    if (!s->dump_file) {
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    fprintf(s->dump_file, "%" PRId64 " fm audio count=%u bits=%u data=",
            now, s->audio_sample_count, (s->audio_flags & 1) ? 16 : 8);
    for (uint16_t i = 0; i < s->audio_bytes_received; i++) {
        fprintf(s->dump_file, "%02X", s->audio_buf[i]);
    }
    fputc('\n', s->dump_file);
    fflush(s->dump_file);
}

static uint32_t fm_transmitter_transfer(SSIPeripheral *dev, uint32_t val)
{
    FMTransmitterState *s = FM_TRANSMITTER(dev);
    uint8_t b = (uint8_t)(val & 0xFF);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->in_audio_packet) {
        if (s->param_idx == 0) {
            s->audio_sample_count = b;
            s->param_idx = 1;
            return FM_RESP_MORE;
        } else if (s->param_idx == 1) {
            s->audio_flags = b;
            s->param_idx = 2;
            uint8_t bytes_per_sample = (s->audio_flags & 1) ? 2 : 1;
            s->audio_bytes_expected = (uint16_t)s->audio_sample_count * bytes_per_sample;
            s->audio_bytes_received = 0;
            return FM_RESP_MORE;
        } else {
            if (s->audio_bytes_received < sizeof(s->audio_buf)) {
                s->audio_buf[s->audio_bytes_received] = b;
            }
            s->audio_bytes_received++;

            if (s->audio_bytes_received >= s->audio_bytes_expected) {
                fm_transmitter_log_audio_packet(s);
                s->in_audio_packet = false;
                s->cmd = 0;
                return FM_RESP_OK;
            }
            return FM_RESP_MORE;
        }
    }

    if (s->cmd == 0) {
        s->cmd = b;
        s->param_idx = 0;
        switch (b) {
        case FM_CMD_NOP:
            s->cmd = 0;
            return FM_RESP_OK;
        case FM_CMD_SET_CARRIER:
            s->param_len = 4;
            return FM_RESP_MORE;
        case FM_CMD_SET_DEVIATION:
        case FM_CMD_SET_SAMPLE_RATE:
            s->param_len = 3;
            return FM_RESP_MORE;
        case FM_CMD_SET_ATTENUATION:
        case FM_CMD_SET_MODE:
            s->param_len = 1;
            return FM_RESP_MORE;
        case FM_CMD_AUDIO_DATA:
            s->in_audio_packet = true;
            s->param_idx = 0;
            s->audio_bytes_received = 0;
            return FM_RESP_MORE;
        default:
            s->cmd = 0;
            return FM_RESP_ERR;
        }
    }

    /* Accumulating parameter bytes for standard commands */
    s->param_buf[s->param_idx++] = b;
    if (s->param_idx >= s->param_len) {
        switch (s->cmd) {
        case FM_CMD_SET_CARRIER:
            s->carrier_hz = ((uint32_t)s->param_buf[0] << 24) |
                            ((uint32_t)s->param_buf[1] << 16) |
                            ((uint32_t)s->param_buf[2] << 8) |
                            (uint32_t)s->param_buf[3];
            if (s->dump_file) {
                fprintf(s->dump_file, "%" PRId64 " fm set-carrier %u\n", now, s->carrier_hz);
                fflush(s->dump_file);
            }
            break;
        case FM_CMD_SET_DEVIATION:
            s->deviation_hz = ((uint32_t)s->param_buf[0] << 16) |
                              ((uint32_t)s->param_buf[1] << 8) |
                              (uint32_t)s->param_buf[2];
            if (s->dump_file) {
                fprintf(s->dump_file, "%" PRId64 " fm set-deviation %u\n", now, s->deviation_hz);
                fflush(s->dump_file);
            }
            break;
        case FM_CMD_SET_SAMPLE_RATE:
            s->sample_rate = ((uint32_t)s->param_buf[0] << 16) |
                             ((uint32_t)s->param_buf[1] << 8) |
                             (uint32_t)s->param_buf[2];
            if (s->dump_file) {
                fprintf(s->dump_file, "%" PRId64 " fm set-rate %u\n", now, s->sample_rate);
                fflush(s->dump_file);
            }
            break;
        case FM_CMD_SET_ATTENUATION:
            s->attenuation_db = s->param_buf[0];
            if (s->dump_file) {
                fprintf(s->dump_file, "%" PRId64 " fm set-attenuation %u\n", now, s->attenuation_db);
                fflush(s->dump_file);
            }
            break;
        case FM_CMD_SET_MODE:
            s->mode = s->param_buf[0];
            if (s->dump_file) {
                fprintf(s->dump_file, "%" PRId64 " fm set-mode %u\n", now, s->mode);
                fflush(s->dump_file);
            }
            break;
        default:
            break;
        }
        s->cmd = 0;
        return FM_RESP_OK;
    }

    return FM_RESP_MORE;
}

static int fm_transmitter_set_cs(SSIPeripheral *dev, bool select)
{
    FMTransmitterState *s = FM_TRANSMITTER(dev);

    if (select) {
        /* CS deasserted (pin driven HIGH) */
        if (s->in_audio_packet && s->audio_bytes_received > 0) {
            fm_transmitter_log_audio_packet(s);
        }
        s->cmd = 0;
        s->param_idx = 0;
        s->in_audio_packet = false;
    }
    return 0;
}

static void fm_transmitter_realize(SSIPeripheral *dev, Error **errp)
{
    FMTransmitterState *s = FM_TRANSMITTER(dev);

    if (s->dump_path) {
        s->dump_file = fopen(s->dump_path, "w");
        if (!s->dump_file) {
            error_setg_errno(errp, errno, "failed to open audio dump file '%s'",
                             s->dump_path);
            return;
        }
    }
}

static void fm_transmitter_reset_hold(Object *obj, ResetType type)
{
    FMTransmitterState *s = FM_TRANSMITTER(obj);

    s->cmd = 0;
    s->param_idx = 0;
    s->param_len = 0;
    s->in_audio_packet = false;
    s->audio_sample_count = 0;
    s->audio_flags = 0;
    s->audio_bytes_expected = 0;
    s->audio_bytes_received = 0;
}

static const Property fm_transmitter_properties[] = {
    DEFINE_PROP_STRING("dump", FMTransmitterState, dump_path),
};

static const VMStateDescription fm_transmitter_vmstate = {
    .name = "fm-transmitter",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, FMTransmitterState),
        VMSTATE_UINT32(carrier_hz, FMTransmitterState),
        VMSTATE_UINT32(deviation_hz, FMTransmitterState),
        VMSTATE_UINT32(sample_rate, FMTransmitterState),
        VMSTATE_UINT8(attenuation_db, FMTransmitterState),
        VMSTATE_UINT8(mode, FMTransmitterState),
        VMSTATE_UINT8(cmd, FMTransmitterState),
        VMSTATE_UINT8(param_idx, FMTransmitterState),
        VMSTATE_UINT8(param_len, FMTransmitterState),
        VMSTATE_UINT8_ARRAY(param_buf, FMTransmitterState, 8),
        VMSTATE_BOOL(in_audio_packet, FMTransmitterState),
        VMSTATE_UINT8(audio_sample_count, FMTransmitterState),
        VMSTATE_UINT8(audio_flags, FMTransmitterState),
        VMSTATE_UINT16(audio_bytes_expected, FMTransmitterState),
        VMSTATE_UINT16(audio_bytes_received, FMTransmitterState),
        VMSTATE_UINT8_ARRAY(audio_buf, FMTransmitterState, 256),
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
