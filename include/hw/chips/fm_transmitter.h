/*
 * FM Radio Transmitter SPI Controller (XMASNGFMv2)
 *
 * Implements the SPI command interface and audio packet streaming
 * receiver for the FM transmitter peripheral.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHIPS_FM_TRANSMITTER_H
#define HW_CHIPS_FM_TRANSMITTER_H

#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_FM_TRANSMITTER "fm-transmitter"
OBJECT_DECLARE_SIMPLE_TYPE(FMTransmitterState, FM_TRANSMITTER)

#define FM_CMD_NOP              0x00
#define FM_CMD_SET_CARRIER      0x01
#define FM_CMD_SET_DEVIATION    0x02
#define FM_CMD_SET_SAMPLE_RATE  0x03
#define FM_CMD_SET_ATTENUATION  0x04
#define FM_CMD_SET_MODE         0x05
#define FM_CMD_AUDIO_DATA       0x10

#define FM_RESP_OK              0x06
#define FM_RESP_MORE            0x04
#define FM_RESP_READY           0x05
#define FM_RESP_ERR             0x15

struct FMTransmitterState {
    SSIPeripheral parent_obj;

    char *dump_path;
    FILE *dump_file;

    uint32_t carrier_hz;
    uint32_t deviation_hz;
    uint32_t sample_rate;
    uint8_t attenuation_db;
    uint8_t mode;

    /* Current transaction parsing */
    uint8_t cmd;
    uint8_t param_idx;
    uint8_t param_len;
    uint8_t param_buf[8];

    /* Audio packet streaming */
    bool in_audio_packet;
    uint8_t audio_sample_count;
    uint8_t audio_flags;
    uint16_t audio_bytes_expected;
    uint16_t audio_bytes_received;
    uint8_t audio_buf[256];
};

#endif
