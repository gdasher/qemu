#!/usr/bin/env python3
"""Integration test for the XMASNGFMv2 FM transmitter firmware on xmasngfm.

Launches QEMU with -M xmasngfm and the prebuilt XMASNGFMv2 firmware,
communicating over the emulated SPI slave chardev socket, and exercises
the full SPI command protocol against the emulated peripherals (MSSP1,
MSSP2, ADF4002, TMR0, NCO1).

Usage: test-xmasngfm.py <path-to-qemu-system-pic16> [path-to-default.hex]
"""

import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_FW = os.path.abspath(
    os.path.join(HERE, '../../../XMASNGFMv2/build/default.hex')
)

# Protocol opcodes
FM_CMD_NOP = 0x00
FM_CMD_SET_CARRIER = 0x01
FM_CMD_SET_DEVIATION = 0x02
FM_CMD_SET_ATTENUATION = 0x03
FM_CMD_SET_SAMPLE_RATE = 0x04
FM_CMD_SET_MODE = 0x05
FM_CMD_AUDIO_DATA = 0x06

# Protocol responses
FM_RESP_READY = 0x00
FM_RESP_MORE = 0x01
FM_RESP_OK = 0x02
FM_RESP_OVERFLOW = 0x03
FM_RESP_ERROR = 0xFF

# Modes
FM_MODE_SILENCE = 0x00
FM_MODE_FM_AUDIO = 0x01
FM_MODE_CW = 0x02
FM_MODE_SINE_TEST = 0x03

# Audio bit depths
FM_AUDIO_BITS_8 = 8
FM_AUDIO_BITS_16 = 16


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    qemu_bin = argv[1]
    fw_hex = argv[2] if len(argv) > 2 else DEFAULT_FW

    if not os.path.isfile(fw_hex):
        print(f"FAIL: Firmware binary not found at {fw_hex}")
        return 1

    failures = []

    def check(name, got, want):
        ok = got == want
        print(f'{"PASS" if ok else "FAIL"} {name}')
        if not ok:
            print(f'     wanted {want:#04x}\n     got    {got:#04x}')
            failures.append(name)

    with tempfile.TemporaryDirectory(prefix='pic16-xmasngfm-') as work:
        sock_path = os.path.join(work, 'spi.sock')

        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(sock_path)
        server.listen(1)

        cmd = [
            qemu_bin,
            '-M', 'xmasngfm',
            '-bios', fw_hex,
            '-display', 'none',
            '-nodefaults',
            '-icount', 'shift=3',
            '-chardev', f'socket,id=spi,path={sock_path}',
            '-serial', 'chardev:spi',
        ]

        qemu_proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        try:
            server.settimeout(5.0)
            conn, _ = server.accept()
            conn.settimeout(2.0)
        except Exception as e:
            print(f"FAIL: Failed to establish SPI socket connection: {e}")
            qemu_proc.kill()
            return 1

        time.sleep(0.1)

        def xfer_byte(b):
            conn.sendall(bytes([b]))
            resp = conn.recv(1)[0]
            time.sleep(0.015)
            return resp

        def spi_cmd(tx_bytes):
            rx = [xfer_byte(b) for b in tx_bytes]
            final = xfer_byte(FM_CMD_NOP)
            return final

        def u32_bytes(val):
            return [(val >> 24) & 0xFF, (val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF]

        def u24_bytes(val):
            return [(val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF]

        # 1. NOP / Status Poll
        check('CMD_NOP returns READY', spi_cmd([FM_CMD_NOP]), FM_RESP_READY)

        # 2. SET_CARRIER: 88.0 MHz, 98.0 MHz, 105.0 MHz, 108.0 MHz
        check(
            'CMD_SET_CARRIER 88.0 MHz',
            spi_cmd([FM_CMD_SET_CARRIER] + u32_bytes(88_000_000)),
            FM_RESP_OK,
        )
        check(
            'CMD_SET_CARRIER 98.0 MHz',
            spi_cmd([FM_CMD_SET_CARRIER] + u32_bytes(98_000_000)),
            FM_RESP_OK,
        )
        check(
            'CMD_SET_CARRIER 105.0 MHz',
            spi_cmd([FM_CMD_SET_CARRIER] + u32_bytes(105_000_000)),
            FM_RESP_OK,
        )
        check(
            'CMD_SET_CARRIER 108.0 MHz',
            spi_cmd([FM_CMD_SET_CARRIER] + u32_bytes(108_000_000)),
            FM_RESP_OK,
        )

        # 3. SET_CARRIER: 50.0 MHz (< 76.0 MHz minimum, invalid)
        check(
            'CMD_SET_CARRIER 50.0 MHz (< 76 MHz rejected)',
            spi_cmd([FM_CMD_SET_CARRIER] + u32_bytes(50_000_000)),
            FM_RESP_ERROR,
        )

        # 4. SET_DEVIATION: 0 Hz, 50 kHz, 75 kHz, 100 kHz
        check(
            'CMD_SET_DEVIATION 0 Hz',
            spi_cmd([FM_CMD_SET_DEVIATION] + u24_bytes(0)),
            FM_RESP_OK,
        )
        check(
            'CMD_SET_DEVIATION 50 kHz',
            spi_cmd([FM_CMD_SET_DEVIATION] + u24_bytes(50_000)),
            FM_RESP_OK,
        )
        check(
            'CMD_SET_DEVIATION 75 kHz',
            spi_cmd([FM_CMD_SET_DEVIATION] + u24_bytes(75_000)),
            FM_RESP_OK,
        )
        check(
            'CMD_SET_DEVIATION 100 kHz',
            spi_cmd([FM_CMD_SET_DEVIATION] + u24_bytes(100_000)),
            FM_RESP_OK,
        )

        # 5. SET_DEVIATION: 300 kHz (> 200 kHz maximum, invalid)
        check(
            'CMD_SET_DEVIATION 300 kHz (> 200 kHz rejected)',
            spi_cmd([FM_CMD_SET_DEVIATION] + u24_bytes(300_000)),
            FM_RESP_ERROR,
        )

        # 6. SET_ATTENUATION: 0 dB, 15 dB, 31 dB
        check(
            'CMD_SET_ATTENUATION 0 dB',
            spi_cmd([FM_CMD_SET_ATTENUATION, 0x00]),
            FM_RESP_OK,
        )
        check(
            'CMD_SET_ATTENUATION 15 dB',
            spi_cmd([FM_CMD_SET_ATTENUATION, 0x0F]),
            FM_RESP_OK,
        )
        check(
            'CMD_SET_ATTENUATION 31 dB',
            spi_cmd([FM_CMD_SET_ATTENUATION, 0x1F]),
            FM_RESP_OK,
        )

        # 7. SET_MODE: FM_AUDIO, SILENCE, CW, SINE_TEST
        check(
            'CMD_SET_MODE FM_AUDIO',
            spi_cmd([FM_CMD_SET_MODE, FM_MODE_FM_AUDIO]),
            FM_RESP_OK,
        )
        check(
            'CMD_SET_MODE SILENCE',
            spi_cmd([FM_CMD_SET_MODE, FM_MODE_SILENCE]),
            FM_RESP_OK,
        )
        check(
            'CMD_SET_MODE CW',
            spi_cmd([FM_CMD_SET_MODE, FM_MODE_CW]),
            FM_RESP_OK,
        )
        check(
            'CMD_SET_MODE SINE_TEST',
            spi_cmd([FM_CMD_SET_MODE, FM_MODE_SINE_TEST]),
            FM_RESP_OK,
        )
        check(
            'CMD_SET_MODE Invalid Mode (0x99 rejected)',
            spi_cmd([FM_CMD_SET_MODE, 0x99]),
            FM_RESP_ERROR,
        )

        # 8. SET_SAMPLE_RATE: 16000, 22050, 32000, 44100, 48000 Hz
        for rate, name in [
            (16000, '16 kHz'),
            (22050, '22.05 kHz'),
            (32000, '32 kHz'),
            (44100, '44.1 kHz'),
            (48000, '48 kHz'),
        ]:
            check(
                f'CMD_SET_SAMPLE_RATE {name}',
                spi_cmd([FM_CMD_SET_SAMPLE_RATE] + u24_bytes(rate)),
                FM_RESP_OK,
            )

        # Unsupported sample rate (11025 Hz)
        check(
            'CMD_SET_SAMPLE_RATE 11025 Hz (unsupported rejected)',
            spi_cmd([FM_CMD_SET_SAMPLE_RATE] + u24_bytes(11025)),
            FM_RESP_ERROR,
        )

        # 9. AUDIO_DATA is streamed in FM_AUDIO mode: in SINE_TEST the
        # firmware keeps the sample queue full of its own tone, so audio
        # would overflow. Move to FM_AUDIO first.
        spi_cmd([FM_CMD_SET_MODE, FM_MODE_FM_AUDIO])

        # 8-bit samples (the current rate is 48 kHz from the loop above).
        samples_8 = [0x10, 0x20, 0x30, 0x40, 0x7F, 0x80, 0xFE, 0x00]
        check(
            'CMD_AUDIO_DATA (8-bit, 8 samples)',
            spi_cmd([FM_CMD_AUDIO_DATA, FM_AUDIO_BITS_8, len(samples_8)] + samples_8),
            FM_RESP_OK,
        )

        # 10. AUDIO_DATA: 16-bit samples. 16-bit packets are only accepted
        # at or below 22.05 kHz (two SPI bytes per sample), so set that rate.
        spi_cmd([FM_CMD_SET_SAMPLE_RATE] + u24_bytes(22050))
        samples_16 = [0x0000, 0x1234, 0x7FFF, 0x8000, 0xC000]
        pkt_16 = [FM_CMD_AUDIO_DATA, FM_AUDIO_BITS_16, len(samples_16)]
        for s in samples_16:
            pkt_16.append((s >> 8) & 0xFF)
            pkt_16.append(s & 0xFF)
        check('CMD_AUDIO_DATA (16-bit, 5 samples)', spi_cmd(pkt_16), FM_RESP_OK)

        # 11b. A 16-bit packet above the 16-bit rate ceiling is rejected at
        # its BITS byte.
        spi_cmd([FM_CMD_SET_SAMPLE_RATE] + u24_bytes(48000))
        # The rejection lands on the BITS byte; spi_cmd's trailing NOP clocks
        # out the response to the last byte sent, so send just the two bytes.
        check(
            'CMD_AUDIO_DATA (16-bit rejected above 22.05 kHz)',
            spi_cmd([FM_CMD_AUDIO_DATA, FM_AUDIO_BITS_16]),
            FM_RESP_ERROR,
        )

        # 11. Invalid Opcode (0xFE)
        check(
            'Invalid Opcode 0xFE (rejected)',
            spi_cmd([0xFE]),
            FM_RESP_ERROR,
        )

        conn.close()
        server.close()
        qemu_proc.kill()
        qemu_proc.wait()

    print()
    if failures:
        print(f"FAILED: {len(failures)} checks failed")
        return 1
    else:
        print("all checks passed")
        return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
