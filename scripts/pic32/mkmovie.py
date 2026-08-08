#!/usr/bin/env python3
#
# Build a movie for the XMASNg firmware, and optionally the SD card image that
# carries it.
#
# A movie is a directory named with a number, holding two files:
#
#   metadata.dat  which logical channels belong to which LED string, in the
#                 "GD" version 1 format the firmware's own parser defines
#   fseq.dat      an FSEQ version 2 sequence: a 32-byte header and then one
#                 frame of channelCount bytes after another
#
# The firmware reads its device ID from a port expander and refuses any region
# addressed to a different one, so --device-id has to match what the board
# presents -- zero, with nothing driving the expander's pins.
#
#   ./mkmovie.py sd --frames 30 --strings 2 --pattern chase
#   ./mkmovie.py sd --image sd.img --qemu-img build/qemu-img
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import os
import struct
import subprocess
import sys

# The firmware's own limits: eight strings of six hundred pixels, and a frame
# period no shorter than 40 ms.
MAX_STRINGS = 8
PIXELS_PER_STRING = 600
MIN_PERIOD_MS = 40

REGION_WS2812 = 0
FSEQ_HEADER_SIZE = 32


def metadata(strings, pixels, device_id):
    """The "GD" region map: one WS2812 region per string, in channel order."""
    out = bytearray(b'GD\x01')
    out += struct.pack('>H', len(strings))
    right = 0
    for channel in strings:
        right += pixels * 3
        out += struct.pack('>HBBB', right, REGION_WS2812, device_id, channel)
    return bytes(out)


def fseq_header(channels, frames, period_ms):
    header = bytearray(FSEQ_HEADER_SIZE)
    header[0:4] = b'PSEQ'
    struct.pack_into('<H', header, 4, FSEQ_HEADER_SIZE)  # channelDataOffset
    header[6] = 0                                        # minorVersion
    header[7] = 2                                        # majorVersion
    struct.pack_into('<H', header, 8, FSEQ_HEADER_SIZE)  # variableDataOffset
    struct.pack_into('<I', header, 10, channels)
    struct.pack_into('<I', header, 14, frames)
    header[18] = period_ms
    header[20] = 0    # compression: none
    header[21] = 0    # compression blocks
    header[22] = 0    # channel ranges: none, so the frame is every channel
    struct.pack_into('<Q', header, 24, 0)  # sequenceUid
    return bytes(header)


def frame(pattern, index, frames, strings, pixels):
    """One frame: three bytes a pixel, strings back to back."""
    data = bytearray(len(strings) * pixels * 3)

    for s in range(len(strings)):
        for p in range(pixels):
            at = (s * pixels + p) * 3
            if pattern == 'solid':
                # Each string a primary, so a decode that crosses strings or
                # swaps colour order is obvious rather than plausible.
                rgb = [(255, 0, 0), (0, 255, 0), (0, 0, 255)][s % 3]
            elif pattern == 'chase':
                # One lit pixel per string, walking along it a pixel a frame.
                lit = (p == (index + s * 7) % pixels)
                rgb = (255, 255, 255) if lit else (0, 0, 0)
            elif pattern == 'gradient':
                level = (p * 255) // max(pixels - 1, 1)
                rgb = (level, (level + index * 8) & 0xFF, 255 - level)
            else:
                rgb = (0, 0, 0)
            data[at:at + 3] = bytes(rgb)
    return bytes(data)


def write_movie(root, number, strings, pixels, frames, period_ms, device_id,
                pattern):
    path = os.path.join(root, str(number))
    os.makedirs(path, exist_ok=True)

    with open(os.path.join(path, 'metadata.dat'), 'wb') as f:
        f.write(metadata(strings, pixels, device_id))

    channels = len(strings) * pixels * 3
    with open(os.path.join(path, 'fseq.dat'), 'wb') as f:
        f.write(fseq_header(channels, frames, period_ms))
        for i in range(frames):
            f.write(frame(pattern, i, frames, strings, pixels))
    return path


def write_image(root, image, qemu_img):
    """
    Turn the directory into a card image. QEMU's SD card model insists on a
    power-of-two size, which the virtual FAT driver's own geometry is not, so
    the image is converted out and then grown to the next one up.
    """
    subprocess.run([qemu_img, 'convert', '-f', 'vvfat', '-O', 'raw',
                    'fat:16:' + root, image], check=True)
    size = os.path.getsize(image)
    rounded = 1 << (size - 1).bit_length()
    subprocess.run([qemu_img, 'resize', '-f', 'raw', image, str(rounded)],
                   check=True, stdout=subprocess.DEVNULL)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('root', help='directory to build the card contents in')
    p.add_argument('--number', type=int, default=0,
                   help='movie number, which is its directory name')
    p.add_argument('--strings', type=int, default=1,
                   help='how many LED strings the movie drives')
    p.add_argument('--pixels', type=int, default=PIXELS_PER_STRING)
    p.add_argument('--frames', type=int, default=4)
    p.add_argument('--period-ms', type=int, default=MIN_PERIOD_MS)
    p.add_argument('--device-id', type=int, default=0)
    p.add_argument('--pattern', default='chase',
                   choices=('solid', 'chase', 'gradient'))
    p.add_argument('--image', help='also build a card image here')
    p.add_argument('--qemu-img', default='qemu-img')
    args = p.parse_args()

    if not 1 <= args.strings <= MAX_STRINGS:
        p.error('strings must be between 1 and %d' % MAX_STRINGS)
    if args.period_ms < MIN_PERIOD_MS:
        p.error('the firmware rejects a period shorter than %d ms'
                % MIN_PERIOD_MS)

    path = write_movie(args.root, args.number, list(range(args.strings)),
                       args.pixels, args.frames, args.period_ms,
                       args.device_id, args.pattern)
    print('%s: %d frames of %d strings x %d pixels'
          % (path, args.frames, args.strings, args.pixels))

    if args.image:
        write_image(args.root, args.image, args.qemu_img)
        print('%s: %d bytes' % (args.image, os.path.getsize(args.image)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
