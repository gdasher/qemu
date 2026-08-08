#!/usr/bin/env python3
"""Play a movie on the emulated board and find where it comes out wrong.

Takes the two files a movie is made of, runs the firmware against them, and
compares what reached the LED strings with what the movie says should have
reached them. Anything that does not match is a glitch, and each one is
reported with the movie frame it belongs to, the string it appeared on, and --
where the shape of the damage says so -- what kind of damage it is:

  torn        the string was latched part way through a frame, so the rest of
              that frame arrived as a second, short one
  shifted     the whole frame arrived rotated by a few pixels
  patched     part of the frame is right and part of it is another frame's
  dropped     a frame in the movie never appeared on the string
  repeated    a frame appeared twice running
  reordered   frames arrived in an order the movie does not have
  corrupt     pixels differ in a way none of the above explains
  late        the frame arrived far outside the movie's frame period

The expected pixels are not guessed. The firmware quantises each colour to
5-6-5 bits and then puts every byte through a gamma table, and that table is
read out of the firmware image itself, so the comparison tracks the image under
test rather than a transcription of it.

Glitches that happen at deterministic points are what this is for, so the
report ends with what the glitch positions have in common: the periods they
fit, where they fall in the movie's loop, and where they fall in the ring
buffer the firmware stages frames through.

  moviecheck.py --qemu build/qemu-system-mipsel --firmware fw.elf \\
                --movie movies/0 --loops 2

  moviecheck.py --movie movies/0 --firmware fw.elf --dump leds.txt

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import collections
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

# The firmware's own limits and layout.
PIXELS_PER_STRING = 600
STRINGS = 8
FSEQ_HEADER_SIZE = 32

# How the firmware stages frames: a ring in the external SRAM, one buffer per
# frame. A glitch whose period is this many frames is the ring wrapping.
SRAM_WORDS = 1047054
FRAME_WORDS = 4803
RING_FRAMES = SRAM_WORDS // FRAME_WORDS

# Region types, from the "GD" format the firmware parses.
REGION_WS2812 = 0
REGION_ONOFF = 1
REGION_EMPTY = 2
REGION_WS2812_RBG = 3

# A frame that arrives more than this far from the movie's period is late.
LATE_TOLERANCE = 0.5


class Movie:
    """A movie as the firmware reads it, and what it should put on the wire."""

    def __init__(self, metadata_path, fseq_path, gamma, device_id=0):
        self.gamma = gamma
        self.device_id = device_id
        self.regions = self._read_metadata(metadata_path)
        self._read_fseq(fseq_path)

    @staticmethod
    def _read_metadata(path):
        with open(path, 'rb') as f:
            data = f.read()
        if len(data) < 10 or data[:3] != b'GD\x01':
            raise ValueError('%s is not a version 1 "GD" region map' % path)
        count = struct.unpack_from('>H', data, 3)[0]
        if len(data) != 5 + 5 * count:
            raise ValueError('%s says %d regions but is %d bytes, not %d'
                             % (path, count, len(data), 5 + 5 * count))
        regions = []
        for i in range(count):
            right, kind, device, channel = struct.unpack_from(
                '>HBBB', data, 5 + 5 * i)
            regions.append({'right': right, 'type': kind, 'device': device,
                            'channel': channel})
        return regions

    def _read_fseq(self, path):
        self.fseq_path = path
        with open(path, 'rb') as f:
            header = f.read(FSEQ_HEADER_SIZE)
        if len(header) < FSEQ_HEADER_SIZE or header[:4] != b'PSEQ':
            raise ValueError('%s is not an FSEQ file' % path)
        if header[7] != 2:
            raise ValueError('%s is version %d, not 2' % (path, header[7]))

        self.data_offset = struct.unpack_from('<H', header, 4)[0]
        self.channels = struct.unpack_from('<I', header, 10)[0]
        self.frames = struct.unpack_from('<I', header, 14)[0]
        self.period_ms = header[18]
        if header[20] & 0xF or header[21] or header[22]:
            raise ValueError('%s is compressed or sparse; the firmware would '
                             'refuse it' % path)

        size = os.path.getsize(path)
        available = (size - self.data_offset) // max(self.channels, 1)
        if available < self.frames:
            raise ValueError('%s claims %d frames but only holds %d'
                             % (path, self.frames, available))

    def frame_data(self, index):
        with open(self.fseq_path, 'rb') as f:
            f.seek(self.data_offset + index * self.channels)
            return f.read(self.channels)

    def transform(self, channel_data):
        """
        What one frame of channel data should put on each string, as the
        firmware works it out: walk the logical channels, follow the regions,
        and quantise. Returns {string: [pixel or None]}, where None is a pixel
        no region covered and so one the firmware never writes.
        """
        out = {}
        k = 0       # logical channel
        r = 0       # region
        n = 0       # pixel within the current region's string

        while k < self.channels:
            if r >= len(self.regions):
                raise ValueError('the regions run out at logical channel %d; '
                                 'the firmware would refuse this movie' % k)
            region = self.regions[r]
            if k >= region['right']:
                r += 1
                n = 0
                continue

            kind = region['type']
            if kind in (REGION_WS2812, REGION_WS2812_RBG):
                if region['device'] != self.device_id:
                    raise ValueError('region %d is for device %d, not %d; the '
                                     'firmware would refuse this movie'
                                     % (r, region['device'], self.device_id))
                string = region['channel']
                pixels = out.setdefault(string, [None] * PIXELS_PER_STRING)
                if n < PIXELS_PER_STRING:
                    pixels[n] = self._pixel(channel_data[k:k + 3],
                                            kind == REGION_WS2812_RBG)
                n += 1
                k += 3
            elif kind == REGION_ONOFF:
                n += 1
                k += 1
            else:
                k += 1
        return out

    def _pixel(self, rgb, rbg):
        """
        One pixel, all the way through: 5-6-5 quantisation, the gamma table,
        and the byte order the string is sent in. The result is what the strip
        decoder reports, so it can be compared with it directly.
        """
        red, green, blue = (rgb + b'\0\0\0')[:3]
        encoded = ((red & 0xF8) >> 3) | ((green & 0xFC) << 3) | \
                  ((blue & 0xF8) << 8)
        red = (encoded & 0x1F) << 3
        green = (encoded & 0x07E0) >> 3
        blue = (encoded & 0xF800) >> 8
        first, second, third = (red, blue, green) if rbg else (red, green, blue)
        return (self.gamma[first] << 16) | (self.gamma[second] << 8) | \
            self.gamma[third]


def read_gamma(firmware):
    """
    The firmware's gamma table, read out of the image under test. Taking it
    from the image rather than from a copy of the source means the comparison
    cannot quietly drift away from the firmware it is checking.
    """
    with open(firmware, 'rb') as f:
        image = f.read()
    if image[:4] != b'\x7fELF' or image[4] != 1:
        return None

    end = '<' if image[5] == 1 else '>'
    e_shoff, = struct.unpack_from(end + 'I', image, 0x20)
    e_shentsize, e_shnum = struct.unpack_from(end + 'HH', image, 0x2E)

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type, = struct.unpack_from(end + 'I', image, off + 4)
        sh_addr, sh_offset, sh_size, sh_link = struct.unpack_from(
            end + 'IIII', image, off + 0x0C)
        sections.append((sh_type, sh_addr, sh_offset, sh_size, sh_link))

    for sh_type, _, sh_offset, sh_size, sh_link in sections:
        if sh_type != 2:    # SHT_SYMTAB
            continue
        _, _, str_off, str_size, _ = sections[sh_link]
        strtab = image[str_off:str_off + str_size]
        for off in range(sh_offset, sh_offset + sh_size, 16):
            st_name, st_value, st_size = struct.unpack_from(end + 'III',
                                                            image, off)
            if not st_name:
                continue
            name = strtab[st_name:strtab.index(b'\0', st_name)]
            if name != b'kGammaLut' or st_size != 256:
                continue
            addr = st_value & 0x1FFFFFFF
            for _, sh_addr, sh_offset2, sh_size2, _ in sections:
                base = sh_addr & 0x1FFFFFFF
                if sh_addr and base <= addr and addr + 256 <= base + sh_size2:
                    at = sh_offset2 + (addr - base)
                    return list(image[at:at + 256])
    return None


def parse_dump(path, strings=None):
    """The LED dump as [(time_ns, string, [pixels])], in the order latched."""
    frames = []
    with open(path) as f:
        for line in f:
            fields = line.split()
            if len(fields) < 2:
                continue
            match = re.fullmatch(r'led(\d+)', fields[1])
            if not match:
                continue
            string = int(match.group(1))
            if strings is not None and string not in strings:
                continue
            pixels = []
            for run in fields[2:]:
                count, colour = run.split('x')
                pixels += [int(colour, 16)] * int(count)
            frames.append((int(fields[0]), string, pixels))
    return frames


def run_machine(args, card, dump, console, wanted):
    """
    Runs the machine until the dump holds the frames asked for. The firmware
    never stops, so something has to decide when enough has been seen; waiting
    for the frames themselves means a slow host takes longer rather than
    truncating the movie.
    """
    argv = [
        args.qemu, '-M', 'pic32mk-devboard,'
        'sdcard=spi1:RD8,'
        'expanders=spi3:RA4:0/spi3:RA4:1,'
        'sram=1048576:0x800000,'
        'leds=RA14:%dx%d:RA1+RB0+RB1:RA11,' % (STRINGS, PIXELS_PER_STRING) +
        'led-order=%s,' % args.led_order +
        'led-dump=' + dump,
        '-drive', 'file=%s,if=sd,format=raw' % card,
        '-bios', args.firmware,
        '-icount', 'shift=%d' % args.icount_shift,
        '-serial', 'file:' + console,
        '-serial', 'null',
        '-display', 'none',
        '-monitor', 'none',
    ]
    if args.verbose:
        print('+ ' + ' '.join(argv), file=sys.stderr)

    open(dump, 'w').close()
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    deadline = time.time() + args.timeout
    seen = 0
    try:
        while time.time() < deadline:
            if proc.poll() is not None:
                break
            time.sleep(1)
            with open(dump) as f:
                seen = sum(1 for _ in f)
            if seen >= wanted:
                break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    if seen == 0:
        stderr = proc.stderr.read().decode(errors='replace') if proc.stderr \
            else ''
        print('nothing reached the LED strings in %d seconds%s'
              % (args.timeout, ('\n' + stderr.strip()) if stderr.strip()
                 else ''), file=sys.stderr)
    return seen


def build_card(movie_dir, tmp, qemu_img, number):
    """A card image carrying the movie, in the directory the firmware wants."""
    root = os.path.join(tmp, 'card')
    target = os.path.join(root, str(number))
    os.makedirs(target)
    for name in ('metadata.dat', 'fseq.dat'):
        shutil.copy(os.path.join(movie_dir, name), target)

    image = os.path.join(tmp, 'card.img')
    subprocess.run([qemu_img, 'convert', '-f', 'vvfat', '-O', 'raw',
                    'fat:16:' + root, image], check=True,
                   stdout=subprocess.DEVNULL)
    size = os.path.getsize(image)
    subprocess.run([qemu_img, 'resize', '-f', 'raw', image,
                    str(1 << (size - 1).bit_length())], check=True,
                   stdout=subprocess.DEVNULL)
    return image


def compare(expected, observed):
    """Which pixel positions differ, ignoring the ones the movie never sets."""
    return [i for i, want in enumerate(expected)
            if want is not None and (i >= len(observed) or observed[i] != want)]


def matches(expected, observed):
    return not compare(expected, observed)


def find_shift(expected, observed):
    """How far the frame is rotated, if that is all that is wrong with it."""
    if len(observed) != len(expected):
        return None
    for shift in range(1, len(expected)):
        rotated = observed[shift:] + observed[:shift]
        if matches(expected, rotated):
            return shift
    return None


def find_patch(expected, others, observed):
    """
    Where a frame stops being itself and becomes another one. That is what a
    buffer read across a frame boundary looks like: right up to some pixel,
    and some other frame's from there on.
    """
    wrong = compare(expected, observed)
    if not wrong:
        return None
    at = wrong[0]
    for index, other in others:
        if other is expected:
            continue
        if not compare(other[at:], observed[at:]):
            return at, index
    return None


class Report:
    def __init__(self):
        self.glitches = []
        self.frames_checked = 0

    def add(self, kind, string, frame, detail, time_ns=None, sequence=None):
        self.glitches.append({'kind': kind, 'string': string, 'frame': frame,
                              'detail': detail, 'time_ns': time_ns,
                              'sequence': sequence})


def analyse(movie, frames, report, args):
    """
    Walks each string's frames in the order they were latched, keeping track of
    where in the movie the firmware should be. Anything that is not the frame
    expected next is looked at more closely before it is called corrupt.
    """
    expected = {}

    def expect(index):
        if index not in expected:
            expected[index] = movie.transform(movie.frame_data(index))
        return expected[index]

    by_string = collections.defaultdict(list)
    for time_ns, string, pixels in frames:
        by_string[string].append((time_ns, pixels))

    for string in sorted(by_string):
        used = [i for i in range(movie.frames)
                if string in expect(i)]
        if not used:
            report.add('unexpected-string', string, None,
                       'the movie has no data for this string, but it was '
                       'driven %d times' % len(by_string[string]))
            continue

        # Where in the movie the first frame seen belongs, so a run that
        # started mid-movie is not reported as one long reordering.
        cursor = 0
        first = by_string[string][0][1]
        for i in used:
            if len(first) == PIXELS_PER_STRING and \
                    matches(expect(i)[string], first):
                cursor = used.index(i)
                break

        previous_time = None
        previous_pixels = None
        for sequence, (time_ns, pixels) in enumerate(by_string[string]):
            index = used[cursor % len(used)]
            want = expect(index)[string]
            report.frames_checked += 1

            if len(pixels) != PIXELS_PER_STRING:
                # A short frame is a frame that was latched early. Say where
                # it was cut, and do not advance: the rest of it is coming.
                report.add('torn', string, index,
                           'latched after %d of %d pixels'
                           % (len(pixels), PIXELS_PER_STRING), time_ns,
                           sequence)
                previous_time = time_ns
                previous_pixels = pixels
                continue

            if matches(want, pixels):
                cursor += 1
            else:
                cursor = handle_mismatch(movie, report, expect, used, cursor,
                                         string, pixels, time_ns, sequence,
                                         args)

            # The firmware stops to read the movie again when it loops, so
            # the frame after the last one is late by design and saying so
            # every time round would bury the times it is not.
            looping = index == used[0]
            if previous_time is not None and not looping:
                gap_ms = (time_ns - previous_time) / 1e6
                if movie.period_ms and \
                        gap_ms > movie.period_ms * (1 + LATE_TOLERANCE) and \
                        gap_ms > args.min_late_ms:
                    report.add('late', string, index,
                               'arrived %.1f ms after the last frame, not %d'
                               % (gap_ms, movie.period_ms), time_ns, sequence)
            previous_time = time_ns
            previous_pixels = pixels


def handle_mismatch(movie, report, expect, used, cursor, string, pixels,
                    time_ns, sequence, args):
    """Works out what kind of wrong a frame is, and where the movie is now."""
    index = used[cursor % len(used)]
    want = expect(index)[string]

    # A frame from somewhere else in the movie: dropped, repeated or out of
    # order, depending on which way it went. The search wraps, because the
    # place a movie is most likely to lose a frame is where it starts again.
    for distance in sorted(range(-args.window, args.window + 1), key=abs):
        candidate = (cursor + distance) % len(used)
        other = used[candidate]
        if not matches(expect(other)[string], pixels):
            continue
        if distance > 0:
            report.add('dropped', string, index,
                       '%d frame(s) never appeared; the next one was %d'
                       % (distance, other), time_ns, sequence)
        elif distance == -1:
            report.add('repeated', string, index,
                       'frame %d arrived twice running' % other, time_ns,
                       sequence)
        else:
            report.add('reordered', string, index,
                       'frame %d arrived again, %d out of place'
                       % (other, -distance), time_ns, sequence)
        return candidate + 1

    shift = find_shift(want, pixels)
    if shift is not None:
        report.add('shifted', string, index,
                   'the whole frame is rotated by %d pixel(s)' % shift,
                   time_ns, sequence)
        return cursor + 1

    others = [(used[(cursor + d) % len(used)],
               expect(used[(cursor + d) % len(used)])[string])
              for d in range(-args.window, args.window + 1)]
    patch = find_patch(want, others, pixels)
    if patch is not None:
        at, other = patch
        report.add('patched', string, index,
                   'right up to pixel %d, then frame %d from there on'
                   % (at, other), time_ns, sequence)
        return cursor + 1

    wrong = compare(want, pixels)
    report.add('corrupt', string, index,
               '%d pixel(s) differ, first at %d: wanted %06X, got %06X'
               % (len(wrong), wrong[0], want[wrong[0]] or 0, pixels[wrong[0]]),
               time_ns, sequence)
    return cursor + 1


def periods(values, tolerance=0.9):
    """Periods that most of these positions fit, longest first."""
    found = []
    if len(values) < 3:
        return found
    span = max(values) - min(values)
    for period in range(2, min(span + 1, 4096)):
        residues = collections.Counter(v % period for v in values)
        common, count = residues.most_common(1)[0]
        if count >= len(values) * tolerance and period <= span:
            found.append((period, common, count))
    # A period that fits is usually accompanied by its multiples; the shortest
    # one is the one that says something.
    keep = []
    for period, residue, count in found:
        if not any(period % p == 0 for p, _, _ in keep):
            keep.append((period, residue, count))
    return keep


def explain_period(period, movie):
    if period == RING_FRAMES:
        return 'the frame ring in the external SRAM wrapping'
    if movie.frames and period == movie.frames:
        return 'the movie looping'
    if movie.frames and period % movie.frames == 0:
        return 'a whole number of movie loops'
    if period == 5:
        return "the firmware's every-fifth-frame path"
    return None


def print_report(report, movie, frames, args):
    print()
    print('checked %d frames on %d string(s) against %d movie frames'
          % (report.frames_checked, len({f[1] for f in frames}), movie.frames))

    if not report.glitches:
        print('no glitches')
        return 0

    kinds = collections.Counter(g['kind'] for g in report.glitches)
    print('%d glitch(es): %s'
          % (len(report.glitches),
             ', '.join('%d %s' % (n, k) for k, n in kinds.most_common())))
    print()

    shown = report.glitches[:args.show]
    for glitch in shown:
        where = 'frame %s' % glitch['frame'] if glitch['frame'] is not None \
            else 'no frame'
        when = ' at %.3f ms' % (glitch['time_ns'] / 1e6) \
            if glitch['time_ns'] else ''
        print('  %-10s string %d, %s%s: %s'
              % (glitch['kind'], glitch['string'], where, when,
                 glitch['detail']))
    if len(report.glitches) > len(shown):
        print('  ... and %d more' % (len(report.glitches) - len(shown)))

    # What the glitches have in common. This is the point of the tool: a fault
    # that repeats at a fixed distance is a fault with a structure behind it.
    positions = [g['frame'] for g in report.glitches if g['frame'] is not None]
    played = [g['sequence'] for g in report.glitches
              if g['sequence'] is not None]
    if len(positions) >= 3:
        print()
        print('where they happen:')

        # Two clocks matter and they are not the same. Where a glitch falls in
        # the movie says the fault is in the movie's own data or in how far
        # through it the firmware is; how many frames have been played says
        # the fault is in something that turns over regardless -- the ring
        # buffer in the SRAM is 218 frames whatever the movie is.
        in_loop = collections.Counter(positions)
        if len(in_loop) <= 5:
            print('  always at movie frame(s): %s'
                  % ', '.join(str(f) for f, _ in in_loop.most_common()))

        gaps = collections.Counter(b - a for a, b in
                                   zip(sorted(set(played)),
                                       sorted(set(played))[1:]))
        if gaps:
            print('  frames played between them: %s'
                  % ', '.join('%d (x%d)' % (g, n)
                              for g, n in gaps.most_common(5)))
        for period, residue, count in periods(played)[:5]:
            note = explain_period(period, movie)
            print('  %d of %d fall every %d frames played, at count %% %d == '
                  '%d%s' % (count, len(played), period, period, residue,
                            ' -- ' + note if note else ''))
        for period, residue, count in periods(positions)[:3]:
            note = explain_period(period, movie)
            print('  %d of %d fall every %d movie frames, at frame %% %d == '
                  '%d%s' % (count, len(positions), period, period, residue,
                            ' -- ' + note if note else ''))

        # Where the frame sits in the file, in case the fault is in reading it
        # rather than in playing it.
        offsets = [movie.data_offset + p * movie.channels for p in positions]
        blocks = collections.Counter(o % 512 for o in offsets)
        if len(blocks) <= 3:
            print('  their data starts %s bytes into a 512-byte card block'
                  % ', '.join(str(b) for b, _ in blocks.most_common()))
    return 1


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--movie', required=True,
                   help='directory holding metadata.dat and fseq.dat')
    p.add_argument('--firmware', required=True,
                   help='the firmware image to run and to read the gamma '
                        'table out of')
    p.add_argument('--qemu', help='qemu-system-mipsel, if the movie is to be '
                                  'played rather than read from a dump')
    p.add_argument('--qemu-img', help='defaults to next to --qemu')
    p.add_argument('--dump', help='analyse this LED dump instead of playing '
                                  'the movie')
    p.add_argument('--keep-dump', help='write the LED dump here and keep it')
    p.add_argument('--loops', type=float, default=2.0,
                   help='how many times through the movie to watch. More than '
                        'one by default, because the point a movie starts '
                        'again is a place faults live')
    p.add_argument('--timeout', type=int, default=1800,
                   help='seconds to let the machine run')
    p.add_argument('--icount-shift', type=int, default=3)
    p.add_argument('--led-order', default='rgb', choices=('rgb', 'grb'))
    p.add_argument('--device-id', type=int, default=0,
                   help='what the board reports as its device ID')
    p.add_argument('--movie-number', type=int, default=0,
                   help='the directory number to put the movie on the card as')
    p.add_argument('--window', type=int, default=8,
                   help='how far to look either side of the expected frame '
                        'when a frame does not match')
    p.add_argument('--show', type=int, default=40,
                   help='how many glitches to describe in full')
    p.add_argument('--min-late-ms', type=float, default=5.0)
    p.add_argument('--verbose', action='store_true')
    args = p.parse_args()

    gamma = read_gamma(args.firmware)
    if gamma is None:
        print('could not read the gamma table from %s; comparing without one'
              % args.firmware, file=sys.stderr)
        gamma = list(range(256))

    movie = Movie(os.path.join(args.movie, 'metadata.dat'),
                  os.path.join(args.movie, 'fseq.dat'), gamma, args.device_id)
    strings = sorted({r['channel'] for r in movie.regions
                      if r['type'] in (REGION_WS2812, REGION_WS2812_RBG)})
    print('%s: %d frames of %d channels every %d ms, on string(s) %s'
          % (args.movie, movie.frames, movie.channels, movie.period_ms,
             ', '.join(str(s) for s in strings)))

    with tempfile.TemporaryDirectory() as tmp:
        if args.dump:
            dump = args.dump
        else:
            if not args.qemu:
                p.error('either --qemu or --dump is needed')
            qemu_img = args.qemu_img or \
                os.path.join(os.path.dirname(os.path.abspath(args.qemu)),
                             'qemu-img')
            card = build_card(args.movie, tmp, qemu_img, args.movie_number)
            dump = args.keep_dump or os.path.join(tmp, 'leds.txt')
            console = os.path.join(tmp, 'console.txt')
            wanted = int(movie.frames * len(strings) * args.loops)
            print('playing %d frame(s), up to %d seconds...'
                  % (wanted, args.timeout))
            if not run_machine(args, card, dump, console, wanted):
                return 2

        frames = parse_dump(dump, set(strings) | set(range(STRINGS)))

    report = Report()
    analyse(movie, frames, report, args)
    return print_report(report, movie, frames, args)


if __name__ == '__main__':
    sys.exit(main())
