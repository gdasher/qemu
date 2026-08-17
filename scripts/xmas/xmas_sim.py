#!/usr/bin/env python3
"""Play a movie through both of the XMAS boards and record what comes off the air.

The product is two microcontrollers. The PIC32MK reads a movie off an SD card,
drives the LED strings, and streams the movie's audio over SPI3 to a second
board; the PIC16 on that board turns the stream into an FM signal. Each half
has been emulated on its own for a while -- the PIC32 against a model of the
transmitter, the PIC16 against a script pretending to be the PIC32 -- and this
runs the two real firmwares against each other instead, and then demodulates
what the radio would have transmitted.

    scripts/xmas/xmas_sim.py --movie ~/movies/5 --out radio.wav

There is no single machine holding both chips, because there is no single QEMU
holding both instruction sets: PIC16 and MIPS are separate targets and separate
binaries. So this starts one of each and joins them with a link that carries
SPI bytes and the virtual time they were clocked at (hw/chips/fm_link.c and
hw/pic16/pic16_cosim_link.c). The PIC32 is the clock master; the PIC16 never
runs past the last moment the PIC32 has reached, so its sample timer drains its
queue at the rate the PIC32's frame timer fills it, and an underrun in the
recording is an underrun the firmware really had.

The PIC16 does not produce a waveform -- it produces an NCO increment per audio
sample, and an LO to mix it against. `rf-dump` writes those out with their
timestamps and scripts/xmas/xmas_superhet.py builds the receiver: two sine
waves, a mixer and an FM detector, running at an offset because nobody can
sample 98 MHz. See that file for how the offset preserves the modulation.

    --rf-only <dir>   demodulate a previous run again without re-emulating it

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# How the XMASNg board is wired. The same description tools/moviecheck.py in
# the XMASNg repository builds, kept here because the machine has no defaults:
# nothing is fitted unless the command line says so.
PIXELS_PER_STRING = 600
STRINGS = 8
RELAY_EXPANDER = 1
FSEQ_HEADER_SIZE = 32

REGION_WS2812 = 0
REGION_ONOFF = 1
REGION_WS2812_RBG = 3

# What the transmitter's protocol looks like on the wire (XMASNGFMv2
# PROTOCOL.md). The link dump records the bytes; this is how to read them.
FM_CMDS = {
    0x00: ('nop', 0),
    0x01: ('carrier', 4),
    0x02: ('deviation', 3),
    0x03: ('attenuation', 1),
    0x04: ('rate', 3),
    0x05: ('mode', 1),
    0x06: ('audio', -1),        # variable: bits, len, then the samples
    0x07: ('status', 0),
    0x08: ('level', 0),
}
FM_MODES = {0: 'silence', 1: 'fm-audio', 2: 'cw', 3: 'sine-test'}
FM_RESP_OVERFLOW = 0x03
FM_RESP_FAULT = 0x04
FM_RESP_ERROR = 0xFF
FM_STATUS_BITS = ((0x01, 'isr-overrun'), (0x02, 'spi-overrun'),
                  (0x04, 'underrun'))


class Movie:
    """Just enough of a movie to run it and to know what it should sound like."""

    def __init__(self, directory, device_id=0):
        self.directory = directory
        self.device_id = device_id
        self.regions = self._read_metadata(os.path.join(directory,
                                                        'metadata.dat'))
        self._read_fseq(os.path.join(directory, 'fseq.dat'))
        self.audio_path = os.path.join(directory, 'audio.wav')
        if not os.path.exists(self.audio_path):
            self.audio_path = None
        self._read_wav()

    @staticmethod
    def _read_metadata(path):
        with open(path, 'rb') as f:
            data = f.read()
        if len(data) < 5 or data[:3] != b'GD\x01':
            raise ValueError('%s is not a version 1 "GD" region map' % path)
        count = struct.unpack_from('>H', data, 3)[0]
        regions = []
        for i in range(count):
            right, kind, device, channel = struct.unpack_from('>HBBB', data,
                                                              5 + 5 * i)
            regions.append({'right': right, 'type': kind, 'device': device,
                            'channel': channel})
        return regions

    def _read_fseq(self, path):
        self.fseq_path = path
        with open(path, 'rb') as f:
            header = f.read(FSEQ_HEADER_SIZE)
        if len(header) < FSEQ_HEADER_SIZE or header[:4] != b'PSEQ':
            raise ValueError('%s is not an FSEQ file' % path)
        self.channels = struct.unpack_from('<I', header, 10)[0]
        self.frames = struct.unpack_from('<I', header, 14)[0]
        self.period_ms = header[18]

    def _read_wav(self):
        self.audio_rate = 0
        self.audio_bits = 0
        self.audio_frames = 0
        if not self.audio_path:
            return
        with wave.open(self.audio_path, 'rb') as w:
            self.audio_rate = w.getframerate()
            self.audio_bits = w.getsampwidth() * 8
            self.audio_frames = w.getnframes()

    def strings(self):
        """Which LED strings this movie drives on this device."""
        return sorted({r['channel'] for r in self.regions
                       if r['type'] in (REGION_WS2812, REGION_WS2812_RBG)
                       and r['device'] == self.device_id})

    def switch_count(self):
        n, left = 0, 0
        for region in self.regions:
            if region['type'] == REGION_ONOFF and \
                    region['device'] == self.device_id:
                n = max(n, region['right'] - left)
            left = region['right']
        return min(n, 8)

    def source_audio(self):
        """The movie's own audio as floats in -1..1, or None."""
        if not self.audio_path:
            return None
        import numpy as np

        with wave.open(self.audio_path, 'rb') as w:
            raw = w.readframes(w.getnframes())
        if self.audio_bits == 16:
            return np.frombuffer(raw, dtype='<i2').astype(np.float64) / 32768.0
        return (np.frombuffer(raw, dtype=np.uint8).astype(np.float64)
                - 128.0) / 128.0


def build_card(movie, work, qemu_img, number=0):
    """A card image carrying the movie, in the directory the firmware wants."""
    root = os.path.join(work, 'card')
    target = os.path.join(root, str(number))
    os.makedirs(target, exist_ok=True)
    for name in ('metadata.dat', 'fseq.dat', 'audio.wav'):
        src = os.path.join(movie.directory, name)
        if os.path.exists(src):
            shutil.copy(src, target)

    image = os.path.join(work, 'card.img')
    subprocess.run([qemu_img, 'convert', '-f', 'vvfat', '-O', 'raw',
                    'fat:16:' + root, image], check=True,
                   stdout=subprocess.DEVNULL)
    size = os.path.getsize(image)
    subprocess.run([qemu_img, 'resize', '-f', 'raw', image,
                    str(1 << (size - 1).bit_length())], check=True,
                   stdout=subprocess.DEVNULL)
    return image


def pic16_argv(args, paths):
    """The transmitter board: its PLL, the link to its master, and its RF out."""
    machine = [
        'pic16-devboard',
        'soc=%s' % args.pic16_soc,
        'adf4002=RC1:RC2:RA6',
        'fm-link=fmlink',
        'rf-dump=' + paths['rf'],
    ]
    return [
        args.qemu_pic16,
        '-M', ','.join(machine),
        '-bios', args.pic16_firmware,
        '-display', 'none', '-nodefaults', '-monitor', 'none',
        # The listening end, and it waits: the master connects when it starts,
        # and until it does this guest has no clock to follow anyway.
        '-chardev', 'socket,id=fmlink,path=%s,server=on,wait=on' % paths['sock'],
        '-icount', 'shift=%d' % args.pic16_icount_shift,
        '-d', 'guest_errors', '-D', paths['pic16_log'],
    ]


def pic32_argv(args, movie, paths):
    """The controller board, wired as the XMASNg product wires it."""
    expanders = 'spi3:RA4:0/spi3:RA4:1' if not args.device_id \
        else 'spi3:RA4:0::%d/spi3:RA4:1' % args.device_id
    machine = [
        'pic32mk-devboard',
        'sdcard=spi1:RD8',
        'expanders=%s' % expanders,
    ]
    switches = movie.switch_count()
    if switches:
        machine.append('relays=%d:%d' % (RELAY_EXPANDER, switches))
    machine.extend([
        'sram=1048576:0x800000',
        'fm=spi3:RD15',
        'fm-link=fmlink',
        'audio-dump=' + paths['fm'],
        'leds=RA14:%dx%d:RA1+RB0+RB1:RA11' % (STRINGS, PIXELS_PER_STRING),
        'led-dump=' + paths['led'],
    ])
    return [
        args.qemu_mipsel,
        '-M', ','.join(machine),
        '-drive', 'file=%s,if=sd,format=raw' % paths['card'],
        '-bios', args.pic32_firmware,
        '-display', 'none', '-monitor', 'none',
        '-serial', 'file:' + paths['console'],
        '-serial', 'null',
        '-chardev', 'socket,id=fmlink,path=%s' % paths['sock'],
        '-icount', 'shift=%d' % args.pic32_icount_shift,
        '-d', 'guest_errors', '-D', paths['pic32_log'],
    ]


def stop(proc):
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def run(args, movie, paths):
    """Runs both machines until the movie has played, and stops them.

    The firmwares never stop on their own, so something has to decide when
    enough has been seen; waiting for latched LED frames means a slow host
    takes longer rather than cutting the movie short.
    """
    strings = len(movie.strings()) or 1
    wanted = args.frames if args.frames else \
        movie.frames * args.loops * strings

    open(paths['led'], 'w').close()
    open(paths['rf'], 'w').close()

    if os.path.exists(paths['sock']):
        os.unlink(paths['sock'])

    if args.verbose:
        print('+ ' + ' '.join(pic16_argv(args, paths)), file=sys.stderr)
    pic16 = subprocess.Popen(pic16_argv(args, paths),
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE)

    deadline = time.time() + 30
    while not os.path.exists(paths['sock']):
        if pic16.poll() is not None or time.time() > deadline:
            stop(pic16)
            raise RuntimeError('the transmitter did not open its link socket'
                               + tail(pic16))
        time.sleep(0.05)

    if args.verbose:
        print('+ ' + ' '.join(pic32_argv(args, movie, paths)), file=sys.stderr)
    pic32 = subprocess.Popen(pic32_argv(args, movie, paths),
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE)

    print('playing %d frame(s) on %d string(s), up to %d seconds...'
          % (wanted, strings, args.timeout))
    seen = 0
    started = time.time()
    deadline = started + args.timeout
    try:
        while time.time() < deadline:
            if pic32.poll() is not None or pic16.poll() is not None:
                break
            time.sleep(1)
            with open(paths['led']) as f:
                seen = sum(1 for line in f if ' led' in line)
            if seen >= wanted:
                break
    finally:
        exited32, exited16 = pic32.poll(), pic16.poll()
        stop(pic32)
        stop(pic16)

    elapsed = time.time() - started
    if exited32 is not None and seen < wanted:
        print('the controller exited (status %s) after %d of %d frame(s)%s'
              % (exited32, seen, wanted, tail(pic32)), file=sys.stderr)
    if exited16 is not None:
        print('the transmitter exited (status %s)%s'
              % (exited16, tail(pic16)), file=sys.stderr)
    for proc in (pic32, pic16):
        if proc.stderr:
            proc.stderr.close()
    return seen, wanted, elapsed


def tail(proc, limit=2000):
    if not proc.stderr:
        return ''
    try:
        text = proc.stderr.read().decode(errors='replace').strip()
    except Exception:
        return ''
    return ('\n' + text[-limit:]) if text else ''


class FmTrace:
    """What crossed the SPI link, read back as the protocol rather than bytes.

    The dump records each exchange as the byte sent and the byte that came
    back, and the transmitter answers a byte during the *next* exchange, so a
    reply belongs to the line before the one it appears on.
    """

    def __init__(self, path):
        self.config = {}
        self.modes = []
        self.packets = 0
        self.samples = 0
        self.overflows = 0
        self.errors = 0
        self.faults = 0
        self.statuses = []
        self.levels = []
        self.bytes = 0
        self.first_audio_ns = None
        self.last_audio_ns = None
        self._parse(path)

    def _parse(self, path):
        if not path or not os.path.exists(path):
            return
        exchanges = []
        with open(path, 'r', errors='replace') as f:
            for line in f:
                fields = line.split()
                if len(fields) < 4 or fields[1] != 'spi':
                    continue
                try:
                    when = int(fields[0])
                    tx = int(fields[2].split('=')[1], 16)
                    rx = int(fields[3].split('=')[1], 16)
                except (ValueError, IndexError):
                    continue
                exchanges.append((when, tx, rx))
        self.bytes = len(exchanges)

        state = None           # (name, bytes still wanted)
        accum = []
        audio = None           # [bits, length, bytes still wanted]
        expect_reply = None    # a read command whose answer is the next byte

        for i, (when, tx, rx) in enumerate(exchanges):
            if rx == FM_RESP_OVERFLOW:
                self.overflows += 1
            elif rx == FM_RESP_ERROR:
                self.errors += 1
            elif rx == FM_RESP_FAULT:
                self.faults += 1

            if expect_reply == 'status':
                self.statuses.append(rx)
            elif expect_reply == 'level':
                self.levels.append(rx)
            expect_reply = None

            if audio is not None:
                if audio[0] is None:
                    audio[0] = tx
                elif audio[1] is None:
                    audio[1] = tx
                    audio[2] = tx * (2 if audio[0] == 0x10 else 1)
                    if audio[2] == 0:
                        audio = None
                else:
                    audio[2] -= 1
                    if audio[2] == 0:
                        self.packets += 1
                        self.samples += audio[1]
                        if self.first_audio_ns is None:
                            self.first_audio_ns = when
                        self.last_audio_ns = when
                        audio = None
                continue

            if state is not None:
                accum.append(tx)
                state = (state[0], state[1] - 1)
                if state[1] == 0:
                    value = 0
                    for b in accum:
                        value = (value << 8) | b
                    self.config[state[0]] = value
                    if state[0] == 'mode':
                        self.modes.append((when, value))
                    state = None
                    accum = []
                continue

            name, count = FM_CMDS.get(tx, (None, 0))
            if name == 'audio':
                audio = [None, None, None]
            elif name in ('status', 'level'):
                expect_reply = name
            elif name and count:
                state = (name, count)
                accum = []

    def status_names(self):
        out = set()
        for value in self.statuses:
            for bit, name in FM_STATUS_BITS:
                if value & bit:
                    out.add(name)
        return sorted(out)


def align(recovered, source, rate):
    """How well the recording matches the movie, and where it starts.

    The recording begins when the transmitter's interrupt does, which is long
    before any audio -- power-on leaves it playing its own test tone -- so the
    two have to be lined up before they can be compared. A couple of seconds
    from the middle of the movie is enough to find the offset, and the score is
    then the normalised correlation over everything that overlaps from there.
    """
    import numpy as np

    if recovered is None or source is None or not len(recovered) or \
            not len(source):
        return None

    window = min(len(source), 2 * rate)
    probe = source[len(source) // 2:len(source) // 2 + window]
    if len(probe) < rate // 2 or len(recovered) < len(probe):
        window = min(len(source), len(recovered)) // 2
        probe = source[:window]
    if len(probe) < 64:
        return None

    probe = probe - probe.mean()
    if not probe.any():
        return None

    # Normalised cross-correlation, by FFT: the recording is long and the probe
    # is short, so anything else takes minutes.
    n = 1 << int(np.ceil(np.log2(len(recovered) + len(probe))))
    corr = np.fft.irfft(np.fft.rfft(recovered, n) *
                        np.conj(np.fft.rfft(probe, n)), n)
    energy = np.sqrt(np.convolve(recovered ** 2, np.ones(len(probe)), 'valid'))
    usable = min(len(corr), len(energy))
    scores = corr[:usable] / (energy[:usable] * np.linalg.norm(probe) + 1e-12)
    lag = int(np.argmax(scores))
    return {'score': float(scores[lag]), 'lag_s': lag / rate,
            'probe_s': len(probe) / rate}


def report(movie, trace, dump, seen, wanted, elapsed, alignment, plan):
    print()
    print('--- the link ---------------------------------------------------')
    print('  %d SPI byte(s) crossed it' % trace.bytes)
    if trace.config:
        carrier = trace.config.get('carrier')
        print('  configured: carrier %s, deviation %s Hz, rate %s Hz, '
              'attenuation %s dB'
              % ('%.3f MHz' % (carrier / 1e6) if carrier else '?',
                 trace.config.get('deviation', '?'),
                 trace.config.get('rate', '?'),
                 trace.config.get('attenuation', '?')))
        print('  modes set: %s'
              % (', '.join(FM_MODES.get(m, str(m)) for _, m in trace.modes)
                 or 'none'))
    else:
        print('  the transmitter was never configured')
    print('  %d audio packet(s), %d sample(s)' % (trace.packets, trace.samples))
    if trace.levels:
        print('  queue level: %d..%d, resting near %d'
              % (min(trace.levels), max(trace.levels),
                 sorted(trace.levels)[len(trace.levels) // 2]))
    faults = trace.status_names()
    print('  faults: %s' % (', '.join(faults) if faults else 'none latched'))
    if trace.overflows:
        print('  %d sample(s) refused because the queue was full' %
              trace.overflows)
    if trace.errors:
        print('  %d byte(s) the transmitter refused' % trace.errors)

    print()
    print('--- the radio --------------------------------------------------')
    print('  %d NCO increment(s) over %.3f s of guest time'
          % (len(dump), dump.duration_ns() / 1e9))
    lo = dump.lo_hz()
    if lo:
        carrier = trace.config.get('carrier')
        print('  local oscillator %.3f MHz%s' % (lo / 1e6,
              ', IF %.3f MHz' % ((lo - carrier) / 1e6) if carrier else ''))
        if dump.lo_changes() > 1:
            print('  the oscillator was retuned %d time(s); the receiver '
                  'follows the last' % dump.lo_changes())
    else:
        print('  the PLL was never programmed')
    gaps = dump.gaps_ns()
    if gaps:
        worst = max(g[1] for g in gaps)
        print('  %d gap(s) in the sample interrupt, the longest %.1f ms'
              % (len(gaps), worst / 1e6))
    if plan:
        print('  receiver: %s' % plan.describe())

    print()
    print('--- the run ----------------------------------------------------')
    print('  %d of %d LED frame(s) in %.1f s of wall time' %
          (seen, wanted, elapsed))
    if alignment:
        print('  the recording matches the movie\'s audio %.3f (%.1f s probe, '
              'found %.2f s in)' % (alignment['score'], alignment['probe_s'],
                                    alignment['lag_s']))
    elif movie.audio_path:
        print('  the recording could not be lined up against the movie')


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.split('\n')[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    qemu_build = os.path.abspath(os.path.join(here, '..', '..', 'build'))

    parser.add_argument('--movie', help='directory holding metadata.dat, '
                                        'fseq.dat and audio.wav')
    parser.add_argument('--out', default='radio.wav',
                        help='where to write the demodulated audio')
    parser.add_argument('--device-id', type=int, default=0,
                        help='what the board is strapped to, which decides '
                             'which of a movie\'s regions are for it')
    parser.add_argument('--loops', type=int, default=1,
                        help='how many times to play the movie through')
    parser.add_argument('--frames', type=int,
                        help='stop after this many latched LED frames instead')
    parser.add_argument('--timeout', type=int, default=1800,
                        help='give up after this many seconds of wall time')

    parser.add_argument('--qemu-mipsel',
                        default=os.path.join(qemu_build, 'qemu-system-mipsel'))
    parser.add_argument('--qemu-pic16',
                        default=os.path.join(qemu_build, 'qemu-system-pic16'))
    parser.add_argument('--qemu-img',
                        default=os.path.join(qemu_build, 'qemu-img'))
    parser.add_argument('--pic32-firmware',
                        default=os.path.expanduser(
                            '~/git/XMASNg/build/XMasNG2.X.production.elf'))
    parser.add_argument('--pic16-firmware',
                        default=os.path.expanduser(
                            '~/git/XMASNGFMv2/build/default.hex'))
    parser.add_argument('--pic16-soc', default='pic16f15355')
    parser.add_argument('--pic32-icount-shift', type=int, default=3,
                        help='1 instruction is 2^shift ns; 3 is about the '
                             'PIC32MK\'s 120 MHz')
    parser.add_argument('--pic16-icount-shift', type=int, default=7,
                        help='7 is the PIC16\'s real 8 MIPS, and the only '
                             'setting at which its interrupt has to fit its '
                             'sample period')

    parser.add_argument('--workdir', help='put the run\'s files here and keep '
                                          'them')
    parser.add_argument('--rf-only', metavar='DIR',
                        help='demodulate a previous run\'s files again '
                             'without emulating anything')
    parser.add_argument('--listen', action='store_true',
                        help='also play the result through the host')
    parser.add_argument('--verbose', action='store_true')
    args = parser.parse_args()

    if not args.rf_only and not args.movie:
        parser.error('--movie is required unless --rf-only names a previous run')

    temp = None
    if args.rf_only:
        work = args.rf_only
    elif args.workdir:
        work = args.workdir
        os.makedirs(work, exist_ok=True)
    else:
        temp = tempfile.mkdtemp(prefix='xmas-sim-')
        work = temp

    paths = {
        'sock': os.path.join(work, 'fm.sock'),
        'rf': os.path.join(work, 'rf.txt'),
        'fm': os.path.join(work, 'fm.txt'),
        'led': os.path.join(work, 'leds.txt'),
        'console': os.path.join(work, 'console.txt'),
        'pic32_log': os.path.join(work, 'pic32.log'),
        'pic16_log': os.path.join(work, 'pic16.log'),
        'card': os.path.join(work, 'card.img'),
    }

    try:
        movie = None
        if args.movie:
            movie = Movie(args.movie, args.device_id)
            print('%s: %d frames of %d channels every %d ms, on string(s) %s%s'
                  % (args.movie, movie.frames, movie.channels, movie.period_ms,
                     ', '.join(str(s) for s in movie.strings()) or 'none',
                     ', with audio.wav (%d Hz, %d-bit)'
                     % (movie.audio_rate, movie.audio_bits)
                     if movie.audio_path else ', with no audio'))

        seen, wanted, elapsed = 0, 0, 0.0
        if not args.rf_only:
            paths['card'] = build_card(movie, work, args.qemu_img)
            seen, wanted, elapsed = run(args, movie, paths)

        from xmas_rf_blocks import RfDump
        from xmas_superhet import Plan, demodulate

        trace = FmTrace(paths['fm'])
        dump = RfDump(paths['rf'])
        if not len(dump):
            print('nothing reached the radio: the transmitter never wrote an '
                  'NCO increment. Its log is %s' % paths['pic16_log'],
                  file=sys.stderr)
            return 1

        rate = trace.config.get('rate') or (movie.audio_rate if movie else 0) \
            or 22050
        deviation = trace.config.get('deviation') or 75000
        carrier = trace.config.get('carrier')

        plan = Plan(rate, deviation)
        demodulate(dump, rate, deviation, args.out, args.listen,
                   carrier_hz=carrier)
        print()
        print('wrote %s' % args.out)

        alignment = None
        if movie and movie.audio_path:
            import numpy as np

            with wave.open(args.out, 'rb') as w:
                got = np.frombuffer(w.readframes(w.getnframes()),
                                    dtype='<i2').astype(np.float64) / 32768.0
            source = movie.source_audio()
            if rate == movie.audio_rate:
                alignment = align(got, source, rate)

        report(movie, trace, dump, seen, wanted, elapsed, alignment, plan)
        if work != temp:
            print()
            print('the run\'s files are in %s' % work)
        return 0
    finally:
        if temp and not args.workdir:
            shutil.rmtree(temp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
