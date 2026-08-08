#!/usr/bin/env python3
"""End-to-end check of the PIC32MK emulation: a movie plays.

Builds a movie whose pattern is one lit pixel walking along each string,
puts it on a virtual SD card, runs the XMASNg firmware against it, and checks
what came out of the LED strings. Between them the checks cover every part of
the machine the firmware uses:

  the console            UART1, and the port pin that keys the transceiver
  the scheduler          the interrupt controller, Timer1, the yield path
  the card               SPI1, the SD card, FatFs on a partitioned image
  the frame buffer       the parallel port and its external SRAM, CRC and all
  the strings            eight WS2812 strips behind a demultiplexer
  the rate               frames arriving at the period the movie asks for

The firmware is not in this repository and cannot be built here, so its path
must be given; without one the test says so and skips. The movie it plays is
built by that firmware's own tools/mkmovie.py, for the same reason -- the
format is the firmware's -- so the checkout is needed rather than just the
image. Both are looked for under XMASNG_DIR, and a checkout somewhere else is
given by setting it.

Usage: test-movie.py <qemu-system-mipsel> [firmware.elf]
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

XMASNG_DIR = os.path.expanduser(os.environ.get('XMASNG_DIR', '~/git/XMASNg'))
MKMOVIE = os.path.join(XMASNG_DIR, 'tools', 'mkmovie.py')

DEFAULT_FIRMWARE = os.path.join(XMASNG_DIR, 'build',
                                'XMasNG2.X.production.elf')

# What the movie says, and so what the machine has to reproduce.
STRINGS = 2
PIXELS = 600
FRAMES = 6
PERIOD_MS = 40

# The movie is generated with a fixed pattern; these are the parts of it that
# survive the firmware's own transform. It quantises each colour to 5-6-5 bits
# and then puts it through a gamma table, so a pixel's exact value is the
# firmware's business -- what it cannot change is which pixel is lit.
LIT_STEP = 7        # how far apart the strings' lit pixels start

# Frames should arrive a period apart. The firmware cannot go faster than it
# can shift the bits out, so a frame that is late is only interesting if it is
# very late.
PERIOD_TOLERANCE = 0.25


def run(argv, **kwargs):
    return subprocess.run(argv, check=True, capture_output=True, text=True,
                          **kwargs)


def build_card(tmp, qemu_img):
    """A movie, and the card image carrying it."""
    root = os.path.join(tmp, 'sd')
    image = os.path.join(tmp, 'sd.img')
    run([sys.executable, MKMOVIE, root,
         '--strings', str(STRINGS), '--pixels', str(PIXELS),
         '--frames', str(FRAMES), '--period-ms', str(PERIOD_MS),
         '--pattern', 'chase', '--image', image, '--qemu-img', qemu_img])
    return image


def run_machine(qemu, firmware, image, tmp, seconds):
    console = os.path.join(tmp, 'console.txt')
    leds = os.path.join(tmp, 'leds.txt')

    argv = [
        qemu, '-M', 'pic32mk-devboard,'
        'sdcard=spi1:RD8,'
        'expanders=spi3:RA4:0/spi3:RA4:1,'
        'sram=1048576:0x800000,'
        'leds=RA14:8x600:RA1+RB0+RB1:RA11,'
        'led-order=rgb,'
        'led-dump=' + leds,
        '-drive', 'file=%s,if=sd,format=raw' % image,
        '-bios', firmware,
        '-icount', 'shift=3',
        '-serial', 'file:' + console,
        '-serial', 'null',
        '-display', 'none',
        '-monitor', 'none',
    ]
    try:
        subprocess.run(argv, timeout=seconds, capture_output=True)
    except subprocess.TimeoutExpired:
        pass    # the firmware never stops; the timeout is how it ends

    with open(console, errors='replace') as f:
        text = f.read()
    with open(leds) as f:
        frames = [parse_frame(line) for line in f if line.strip()]
    return text, frames


def parse_frame(line):
    """"<time> led<n> <count>x<RRGGBB> ..." into (time, string, [pixels])."""
    fields = line.split()
    time_ns = int(fields[0])
    string = int(re.fullmatch(r'led(\d+)', fields[1]).group(1))
    pixels = []
    for run_spec in fields[2:]:
        count, colour = run_spec.split('x')
        pixels += [int(colour, 16)] * int(count)
    return time_ns, string, pixels


def check(results, name, ok, detail=''):
    results.append((name, ok, detail))
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name,
                       '' if ok or not detail else ': ' + detail))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    qemu = sys.argv[1]
    firmware = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_FIRMWARE
    qemu_img = os.path.join(os.path.dirname(qemu), 'qemu-img')

    if not os.path.exists(firmware):
        print('SKIP: no firmware at %s' % firmware)
        return 0
    if not os.path.exists(MKMOVIE):
        print('SKIP: no XMASNg checkout at %s; set XMASNG_DIR' % XMASNG_DIR)
        return 0
    if not os.path.exists(qemu_img):
        print('SKIP: no qemu-img next to %s' % qemu)
        return 0

    results = []
    with tempfile.TemporaryDirectory() as tmp:
        image = build_card(tmp, qemu_img)
        console, frames = run_machine(qemu, firmware, image, tmp, 240)

    for line in ('App Start Done', 'Parsed meta', 'Parsed fseq',
                 'Loaded First Frame'):
        check(results, 'console says %r' % line, line in console)

    check(results, 'the strings were driven', bool(frames),
          'nothing reached the LED strings')
    if not frames:
        return 1

    strings = sorted({f[1] for f in frames})
    check(results, 'every string in the movie was driven',
          strings == list(range(STRINGS)),
          'saw %s, wanted %s' % (strings, list(range(STRINGS))))

    check(results, 'frames are a whole string long',
          all(len(f[2]) == PIXELS for f in frames),
          'lengths %s' % sorted({len(f[2]) for f in frames}))

    # The pattern lights one pixel and moves it along by one a frame, so a
    # frame with two lit pixels means two frames were run together, and one
    # with none means a frame was cut in half.
    lit = [[i for i, c in enumerate(f[2]) if c] for f in frames]
    check(results, 'each frame lights exactly one pixel',
          all(len(x) == 1 for x in lit),
          'counts %s' % sorted({len(x) for x in lit}))

    if all(len(x) == 1 for x in lit):
        for string in strings:
            walk = [x[0] for f, x in zip(frames, lit) if f[1] == string]
            steps = [(b - a) % PIXELS for a, b in zip(walk, walk[1:])]
            # The movie is short and loops, so the pixel walks forward a frame
            # at a time and then jumps back where it starts again. Every step
            # that is not a jump back has to be exactly one.
            forward = [n for n in steps if n < PIXELS // 2]
            check(results, 'string %d advances a pixel a frame' % string,
                  forward and set(forward) == {1},
                  'steps %s' % sorted(set(steps)))
            check(results, 'string %d played the whole movie' % string,
                  len(forward) >= FRAMES - 2,
                  'only %d frames in a row' % len(forward))

        # The strings start their chases a fixed distance apart, so a
        # demultiplexer that sent every string the same data -- or sent one
        # string's data to another -- would show up here.
        pairs = [(a, b) for a, b in zip(frames, frames[1:])
                 if a[1] == 0 and b[1] == 1]
        offsets = {(b[2].index(next(c for c in b[2] if c)) -
                    a[2].index(next(c for c in a[2] if c))) % PIXELS
                   for a, b in pairs}
        check(results, 'each string gets its own data',
              offsets == {LIT_STEP},
              'string 1 leads string 0 by %s, wanted %d'
              % (sorted(offsets), LIT_STEP))

    first = [f for f in frames if f[1] == strings[0]]
    gaps = sorted((b[0] - a[0]) / 1e6 for a, b in zip(first, first[1:]))
    if gaps:
        # The firmware cannot start a frame before it has finished shifting
        # the last one out, and it stops to read the card again when the movie
        # loops, so what must hold is that the usual gap is steady and is the
        # period the movie asked for.
        median = gaps[len(gaps) // 2]
        steady = [g for g in gaps
                  if abs(g - median) <= PERIOD_TOLERANCE * median]
        check(results, 'frames arrive at the movie\'s rate',
              abs(median - PERIOD_MS) <= PERIOD_TOLERANCE * PERIOD_MS,
              'the usual gap is %.1f ms, wanted %d' % (median, PERIOD_MS))
        # One gap in every loop of the movie is the pause to read the card
        # again, so most rather than all of them are the period.
        check(results, 'the rate is steady',
              len(steady) * 3 >= len(gaps) * 2,
              '%d of %d gaps are near %.1f ms; the others run to %.1f'
              % (len(steady), len(gaps), median, max(gaps)))

    failed = [name for name, ok, _ in results if not ok]
    print()
    print('%d/%d checks passed' % (len(results) - len(failed), len(results)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
