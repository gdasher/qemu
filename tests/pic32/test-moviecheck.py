#!/usr/bin/env python3
"""Check that moviecheck.py recognises the damage it claims to recognise.

The tool's job is to say what kind of wrong a frame is, and that judgement is
the part most likely to be quietly wrong -- a classifier that calls everything
"corrupt" still looks like it is working. So this builds a movie, synthesises
the LED dump a machine with no faults would produce, damages it in one specific
way at a time, and checks the tool names the damage.

It does not need an emulator, and it does not check the tool's model of what
the firmware does to a colour: the dump is synthesised from that same model, so
the two agree by construction. What proves the model is tests/pic32/test-movie.py,
which compares it against the real machine.

Usage: test-moviecheck.py
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(HERE, '..', '..', 'scripts', 'pic32')
sys.path.insert(0, SCRIPTS)

import mkmovie          # noqa: E402
import moviecheck       # noqa: E402

STRINGS = 2
PIXELS = moviecheck.PIXELS_PER_STRING
FRAMES = 8
PERIOD_MS = 40
PERIOD_NS = PERIOD_MS * 1000 * 1000


def build_movie(root):
    return mkmovie.write_movie(root, 0, list(range(STRINGS)), PIXELS, FRAMES,
                               PERIOD_MS, 0, 'chase')


def perfect_dump(movie, loops=2):
    """The frames a machine with nothing wrong with it would latch."""
    lines = []
    time_ns = 1000 * 1000 * 1000
    for loop in range(loops):
        for index in range(movie.frames):
            strings = movie.transform(movie.frame_data(index))
            for string in sorted(strings):
                lines.append((time_ns, string, list(strings[string])))
                time_ns += PERIOD_NS // max(len(strings), 1)
    return lines


def write_dump(path, lines):
    with open(path, 'w') as f:
        for time_ns, string, pixels in lines:
            runs = []
            i = 0
            while i < len(pixels):
                run = 1
                while i + run < len(pixels) and pixels[i + run] == pixels[i]:
                    run += 1
                runs.append('%ux%06X' % (run, pixels[i] or 0))
                i += run
            f.write('%d led%u %s\n' % (time_ns, string, ' '.join(runs)))


def run_tool(movie_dir, firmware, dump):
    argv = [sys.executable, os.path.join(SCRIPTS, 'moviecheck.py'),
            '--movie', movie_dir, '--firmware', firmware, '--dump', dump]
    out = subprocess.run(argv, capture_output=True, text=True)
    return out.stdout + out.stderr


def kinds_in(output):
    """The glitch kinds the tool named, from its summary line."""
    for line in output.splitlines():
        if 'glitch(es):' in line:
            after = line.split('glitch(es):', 1)[1]
            return {part.strip().split(' ', 1)[1]
                    for part in after.split(',') if part.strip()}
    return set()


# Each case damages the perfect dump in one way and says what should be seen.
def damage_torn(lines):
    time_ns, string, pixels = lines[6]
    lines[6] = (time_ns, string, pixels[:137])
    return 'torn'


def damage_shifted(lines):
    time_ns, string, pixels = lines[6]
    # Rotated far enough that it cannot be mistaken for the next frame, which
    # in a chase pattern is the same thing rotated by one.
    lines[6] = (time_ns, string, pixels[100:] + pixels[:100])
    return 'shifted'


def damage_patched(lines):
    # The splice has to fall between the two frames' lit pixels, or the join
    # produces a frame that is still one of them and there is nothing to find.
    # In a chase pattern frame f lights pixel f, so cutting frame 3 at pixel 4
    # and continuing with frame 6 gives a frame lit in two places.
    time_ns, string, pixels = lines[3 * STRINGS]
    later = lines[6 * STRINGS][2]
    lines[3 * STRINGS] = (time_ns, string, pixels[:4] + later[4:])
    return 'patched'


def damage_dropped(lines):
    del lines[6:6 + STRINGS]
    return 'dropped'


def damage_repeated(lines):
    lines[6:6] = [lines[6 - STRINGS]]
    return 'repeated'


def damage_reordered(lines):
    lines[6:6] = [lines[6 - 4 * STRINGS]]
    return 'reordered'


def damage_corrupt(lines):
    time_ns, string, pixels = lines[6]
    broken = list(pixels)
    for i in range(0, len(broken), 3):
        broken[i] = 0x123456
    lines[6] = (time_ns, string, broken)
    return 'corrupt'


def damage_late(lines):
    # Everything after this frame arrives a quarter of a second later than it
    # should, which is a stall rather than a lost frame.
    for i in range(6, len(lines)):
        time_ns, string, pixels = lines[i]
        lines[i] = (time_ns + 250 * 1000 * 1000, string, pixels)
    return 'late'


CASES = [damage_torn, damage_shifted, damage_patched, damage_dropped,
         damage_repeated, damage_reordered, damage_corrupt, damage_late]


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        movie_dir = build_movie(tmp)

        # The tool reads its gamma table out of a firmware image; with no image
        # to hand it says so and compares without one, which is what this uses.
        firmware = os.path.join(tmp, 'not-an-elf')
        with open(firmware, 'wb') as f:
            f.write(b'not an ELF')

        movie = moviecheck.Movie(os.path.join(movie_dir, 'metadata.dat'),
                                 os.path.join(movie_dir, 'fseq.dat'),
                                 list(range(256)))
        clean = perfect_dump(movie)

        dump = os.path.join(tmp, 'leds.txt')
        write_dump(dump, clean)
        output = run_tool(movie_dir, firmware, dump)
        ok = 'no glitches' in output
        print('%s an undamaged dump is clean' % ('PASS' if ok else 'FAIL'))
        if not ok:
            failures += 1
            print(output)

        for case in CASES:
            lines = [(t, s, list(p)) for t, s, p in clean]
            want = case(lines)
            write_dump(dump, lines)
            output = run_tool(movie_dir, firmware, dump)
            found = kinds_in(output)
            ok = want in found
            print('%s %s is reported as %s%s'
                  % ('PASS' if ok else 'FAIL', case.__name__[len('damage_'):],
                     want, '' if ok else ', but found %s' % (found or 'none')))
            if not ok:
                failures += 1

    print()
    print('%d/%d checks passed' % (len(CASES) + 1 - failures, len(CASES) + 1))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
