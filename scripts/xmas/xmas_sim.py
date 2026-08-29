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
# PROTOCOL.md, version 2: the carrier goes as the PLL divider N and the
# IF's NCO increment, the deviation as the increment scale k). The link
# dump records the bytes; this is how to read them.
FM_CMDS = {
    0x00: ('nop', 0),
    0x01: ('carrier', 5),
    0x02: ('deviation', 2),
    0x03: ('attenuation', 1),
    0x04: ('rate', 3),
    0x05: ('mode', 1),
    0x06: ('audio', -1),        # variable: bits, len, then the samples
    0x07: ('status', 0),
    0x08: ('level', 0),
    0x09: ('osctune', 1),
}
FM_MODES = {0: 'silence', 1: 'fm-audio', 2: 'cw', 3: 'sine-test'}
FM_MODE_FM_AUDIO = 1
FM_RESP_OVERFLOW = 0x03
FM_RESP_FAULT = 0x04
FM_RESP_APPLYING = 0x06     # NOP's answer while staged work is running
FM_RESP_ERROR = 0x0E
FM_RESP_EMPTY = 0xFF        # not a reply: the wire's idle level
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

    def has_regions(self):
        """Whether this movie drives anything on this device.

        REGION_EMPTY (2) is a gap in the channel map and carries no device;
        a movie whose real regions all name another board is one this board
        rejects frame by frame.
        """
        return any(r['device'] == self.device_id and r['type'] != 2
                   for r in self.regions)

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
    if getattr(args, 'pic16_clock_ppm', 0):
        machine.append('osc-ppm=%d' % args.pic16_clock_ppm)
    if getattr(args, 'pic16_drift', None):
        machine.append('drift-profile=%s' % args.pic16_drift.replace(',', ':'))
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
        # Name the dump's colours as moviecheck does, so its --dump reads
        # this run; the strip model's own default is grb.
        'led-order=rgb',
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


def wire_samples(payload, bits):
    """The samples a packet's bytes carry: big-endian signed on the wire.

    Twelve-bit packets carry two samples in three bytes, six nibbles most
    significant first, and are returned left-justified into sixteen --
    which is what they are: the transmitter scales a 12-bit sample with
    the same nibble tables it uses for the top three nibbles of a 16-bit
    one, so the two are the same number to it.
    """
    if bits == 16:
        return [int.from_bytes(bytes(payload[i:i + 2]), 'big', signed=True)
                for i in range(0, len(payload) - 1, 2)]
    if bits == 12:
        out = []
        i = 0
        while i + 1 < len(payload):
            vals = [(payload[i] << 4) | (payload[i + 1] >> 4)]
            if i + 2 < len(payload):
                vals.append(((payload[i + 1] & 0x0F) << 8) | payload[i + 2])
            for v in vals:
                v <<= 4
                out.append(v - 65536 if v > 32767 else v)
            if len(vals) == 1:
                break        # an odd count's lone last sample: two bytes
            i += 3
        return out
    return [b - 256 if b > 127 else b for b in payload]


def wire_bytes(count, bits):
    """How many bytes a packet of `count` samples at this depth takes.

    Rounded up: an odd 12-bit count ends on a lone sample, whose three
    nibbles take two bytes with the fourth unused.
    """
    return (count * bits + 7) // 8


def movie_samples(movie):
    """The movie's audio at the wire's depth -- what the master reads, and
    what deemphasize() recovers from what it sent."""
    import numpy as np

    raw = movie.source_audio()
    if raw is None:
        return []
    if movie.audio_bits == 16:
        return list(np.round(raw * 32768.0).astype(np.int32).clip(-32768, 32767))
    return list(np.round(raw * 128.0).astype(np.int32).clip(-128, 127))


# The master pre-emphasizes what it streams -- the 75 us US broadcast filter,
# y[n] = x[n] - round(alpha * x[n-1]) with alpha = exp(-1/(rate * 75us)) in
# Q15, saturated at the depth's rails, one sample of state carried across a
# stream's frames (FM_Preemph in the controller's fm_master.h). So the wire
# carries the filtered stream, not the movie's samples.
PREEMPH_ALPHA_Q15 = {16000: 14241, 22050: 17899, 32000: 21602,
                     44100: 24218, 48000: 24821}


def deemphasize(samples, bits, rate, resets=()):
    """Undo the master's pre-emphasis, bit-exactly (the same Q15 arithmetic,
    run backwards), so the wire's samples compare against the movie's own.
    `resets`: indices where a stream started and the filter began afresh.
    Exact wherever the master's filter did not clip; a clip leaves a residue
    that decays by alpha a sample."""
    alpha = PREEMPH_ALPHA_Q15.get(rate, 0)
    hi = 32767 if bits == 16 else 127
    resets = set(resets)
    out = []
    prev = 0
    for i, y in enumerate(samples):
        if i in resets:
            prev = 0
        x = y + ((alpha * prev + (1 << 14)) >> 15)
        x = max(-hi - 1, min(hi, x))
        prev = x
        out.append(x)
    return out


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
        self.sample_count = 0
        self.samples = []      # every sample clocked out, in order
        self.depths = set()    # the bit depths the stream used
        self.stream_starts = []  # indices into samples where a stream began
        self.movie_start = 0   # where the movie's own stream begins; see below
        self.overflows = 0
        self.errors = 0
        self.faults = 0
        self.polls = 0
        self.stale = 0
        self.statuses = []
        self.levels = []
        self.bytes = 0
        self.first_audio_ns = None
        self.last_audio_ns = None
        self._parse(path)

    def _parse(self, path):
        if not path or not os.path.exists(path):
            return
        # The select edge is part of the protocol, not decoration: the
        # module resets its parser on it (PROTOCOL.md), so a transaction cut
        # short -- a burst the master aborted, a frame that ended early --
        # costs exactly its own bytes. A reader that ignores the edge parses
        # the next frame's command as the last one's payload and never
        # recovers, which reads as a wire carrying nonsense.
        exchanges = []
        with open(path, 'r', errors='replace') as f:
            for line in f:
                fields = line.split()
                if len(fields) < 3:
                    continue
                try:
                    when = int(fields[0])
                except ValueError:
                    continue
                if fields[1] == 'cs':
                    if fields[2] == '0':
                        exchanges.append((when, None, None))
                    continue
                if len(fields) < 4 or fields[1] != 'spi':
                    continue
                try:
                    tx = int(fields[2].split('=')[1], 16)
                    rx = int(fields[3].split('=')[1], 16)
                except (ValueError, IndexError):
                    continue
                exchanges.append((when, tx, rx))
        self.bytes = sum(1 for _, tx, _ in exchanges if tx is not None)

        state = None           # (name, bytes still wanted)
        accum = []
        audio = None           # [bits, length, bytes still wanted]
        payload = []           # the bytes of the packet in flight
        expect_reply = None    # a read command whose answer is the next byte
        was_audio = False      # the byte this reply belongs to was a sample

        for i, (when, tx, rx) in enumerate(exchanges):
            if tx is None:
                # The select rose: back to idle, wherever this left off.
                state = None
                accum = []
                audio = None
                payload = []
                expect_reply = None
                continue
            if rx == FM_RESP_OVERFLOW:
                self.overflows += 1
            elif rx == FM_RESP_FAULT:
                self.faults += 1
            elif rx == FM_RESP_APPLYING:
                # An accepted carrier or deviation runs its staged work
                # behind the OK; these are the master waiting it out.
                self.polls += 1
            elif rx == FM_RESP_ERROR:
                self.errors += 1
            elif rx == FM_RESP_EMPTY:
                # Not a reply at all: the transmit register was never
                # loaded. Against a sample byte that costs nothing (the
                # master does not read those replies); against a command
                # the master resends the frame, and version 2 moved the
                # refusal off 0xFF exactly so the two cannot be confused.
                self.stale += 1

            was_audio = audio is not None and audio[2] is not None

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
                    audio[2] = wire_bytes(tx, audio[0])
                    if audio[2] == 0:
                        audio = None
                else:
                    payload.append(tx)
                    audio[2] -= 1
                    if audio[2] == 0:
                        self.packets += 1
                        self.sample_count += audio[1]
                        self.samples += wire_samples(payload, audio[0])
                        self.depths.add(audio[0])
                        payload = []
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
                    if state[0] == 'carrier':
                        # A carrier is the signature of a real stream: the
                        # master's boot calibration paces silence bursts at
                        # whatever the module powered up with and never tunes
                        # it. So everything clocked before the first
                        # SET_CARRIER is the calibration's, not the movie's,
                        # and comparing it against the movie's samples is how
                        # a perfect wire reads as an entirely wrong one.
                        self.movie_start = len(self.samples)
                        # [N1 N0 I2 I1 I0]: the PLL divider and the IF's
                        # NCO increment; the RF they encode, for reading.
                        n = value >> 24
                        inc = value & 0xFFFFFF
                        self.config['carrier_n'] = n
                        self.config['carrier_inc'] = inc
                        self.config['carrier'] = n * 100000 - inc * 15625 // 512
                    elif state[0] == 'deviation':
                        # The increment scale k; the peak deviation it
                        # encodes is exact, where the Hz asked for was not.
                        self.config['deviation_k'] = value
                        self.config['deviation'] = value * 15625 // 512
                    else:
                        self.config[state[0]] = value
                    if state[0] == 'mode':
                        self.modes.append((when, value))
                        if value == FM_MODE_FM_AUDIO:
                            self.stream_starts.append(len(self.samples))
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
    two have to be lined up before they can be compared.

    The score is taken over short windows and reported as their median, not
    over the whole stream at once. The master repeats and drops samples to
    hold the transmitter's queue -- that is the pacing loop reconciling its
    frame clock with the transmitter's sample clock, and it is the loop
    working rather than failing -- so the recording is the movie on a
    slightly edited time base. Compared end to end that reads as a poor
    match however good it sounds; compared over a window short enough that
    the edits have not accumulated, it reads as what it is. Whether the
    samples themselves are the movie's is a separate question, and the link
    dump answers it exactly.
    """
    import numpy as np

    if recovered is None or source is None or not len(recovered) or \
            not len(source):
        return None

    # The probe has to be a stretch of the movie the recording could
    # actually contain. A run stopped early -- --frames, or the wall
    # timeout -- holds only the movie's opening, so a probe taken from
    # the middle of a long movie is nowhere in it and the search locks
    # onto noise: the score then says the audio is wrong when it is
    # perfect. Take it from the middle of what was recorded instead.
    window = min(len(source), 2 * rate)
    start = min(len(source) // 2, max(0, (len(recovered) - window) // 2))
    probe = source[start:start + window]
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

    # Window by window from there, each realigned by up to a few samples so a
    # filter's group delay is not counted as a mismatch.
    width = max(64, rate // 50)          # 20 ms
    reach = max(4, rate // 500)
    got = recovered[lag:lag + len(probe)]
    windows = []
    for at in range(0, len(got) - width, width):
        a, b = got[at:at + width], probe[at:at + width]
        if not a.any() or not b.any():
            continue
        windows.append(max(float(np.corrcoef(np.roll(a, d), b)[0, 1])
                           for d in range(-reach, reach + 1)))
    if not windows:
        return None
    return {'score': float(np.median(windows)), 'lag_s': lag / rate,
            'probe_s': len(probe) / rate, 'windows': len(windows),
            'window_ms': 1000.0 * width / rate}


def check_wire(trace, movie):
    """Whether the samples the transmitter was given were the movie's.

    This is the question the recording cannot answer on its own. The master
    repeats and drops samples on purpose, to hold the transmitter's queue
    against the mismatch between its frame clock and the transmitter's sample
    clock, so the stream is the movie on an edited time base -- and an edit
    is the pacing working. A sample that is not the movie's at all is not.

    The wire carries the pre-emphasized stream, so it is de-emphasized --
    the master's filter run backwards, bit-exactly -- before the comparison.
    Only the movie's own stream is compared: the boot calibration's silence
    bursts are the master measuring the module's clock, not the movie, and
    counting them makes a perfect wire read as a wholly wrong one.
    """
    want = movie_samples(movie) if movie else []
    begin = trace.movie_start
    if not want or len(trace.samples) <= begin:
        return None
    rate = trace.config.get('rate', movie.audio_rate)
    got = deemphasize(trace.samples[begin:], movie.audio_bits, rate,
                      [s - begin for s in trace.stream_starts if s >= begin])
    depth = max(trace.depths) if trace.depths else movie.audio_bits
    tol = depth_tolerance(depth, movie.audio_bits, rate)
    passes = len(got) // len(want) + 2
    repeats, skips, wrong = align_samples(got, want * passes, tol=tol)
    return {'repeats': repeats, 'skips': skips, 'wrong': len(wrong),
            'total': len(got), 'depth': depth, 'tol': tol}


def align_samples(received, expected, lookahead=8, tol=0):
    """Line the wire's samples up against the movie's, allowing the two edits
    the master makes -- a sample repeated, or samples skipped -- and nothing
    else. Returns (repeats, skips, indices that matched nothing).

    `tol` is how far a sample may sit from the movie's and still be it: zero
    where the wire carries the movie's own depth, and the quantisation the
    wire's depth imposes where it does not (see check_wire).
    """
    def same(a, b):
        return a == b if tol == 0 else abs(a - b) <= tol

    j = repeats = skips = 0
    wrong = []
    n = len(expected)
    for i, sample in enumerate(received):
        if j < n and same(sample, expected[j]):
            j += 1
            continue
        if 0 < j <= n and same(sample, expected[j - 1]):
            repeats += 1
            continue
        for k in range(1, lookahead + 1):
            if j + k < n and same(sample, expected[j + k]):
                skips += k
                j += k + 1
                break
        else:
            wrong.append(i)
            j += 1
    return repeats, skips, wrong


def depth_tolerance(depth, movie_bits, rate):
    """How far a de-emphasized wire sample can sit from the movie's.

    Zero when the wire carries the movie's own depth. When it carries
    fewer bits -- 12 for a 16-bit movie -- each sample arrives truncated
    to that grid, and de-emphasis (a leaky integrator, x[n] = y[n] +
    alpha*x[n-1]) accumulates that error to at most a step over 1 - alpha
    before it decays. Rounded up, with a little margin.
    """
    if not depth or depth >= movie_bits:
        return 0
    step = 1 << (movie_bits - depth)
    alpha = PREEMPH_ALPHA_Q15.get(rate, 0) / 32768.0
    return int(step / max(1.0 - alpha, 0.05)) + 2


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
    print('  %d audio packet(s), %d sample(s)' % (trace.packets,
                                                  trace.sample_count))
    wire = check_wire(trace, movie)
    if wire:
        within = '' if not wire['tol'] else \
            ' (to within the %d bit(s) the %d-bit wire drops)' % (
                movie.audio_bits - wire['depth'], wire['depth'])
        if wire['wrong']:
            print('  %d of them are not the movie\'s%s'
                  % (wire['wrong'], within))
        else:
            print('  every one of them the movie\'s, in order%s' % within)
        print('  %d repeated and %d dropped to hold the queue (%.2f%% of the '
              'stream)' % (wire['repeats'], wire['skips'],
                           100.0 * (wire['repeats'] + wire['skips']) /
                           max(wire['total'], 1)))
    if trace.levels:
        print('  queue level: %d..%d, resting near %d'
              % (min(trace.levels), max(trace.levels),
                 sorted(trace.levels)[len(trace.levels) // 2]))
    faults = trace.status_names()
    print('  faults: %s' % (', '.join(faults) if faults else 'none latched'))
    if trace.overflows:
        print('  %d sample(s) refused because the queue was full' %
              trace.overflows)
    if trace.polls:
        print('  %d exchange(s) spent waiting for a setting to be applied'
              % trace.polls)
    if trace.stale:
        print('  %d sample byte(s) it had not answered by the time the next '
              'arrived (%.0f%% of the stream; the master does not read those '
              'replies)' % (trace.stale,
                            100.0 * trace.stale / max(trace.bytes, 1)))
    if trace.errors:
        print('  %d command byte(s) the transmitter refused' % trace.errors)

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
        print('  the recording matches the movie\'s audio %.3f, the median of '
              '%d windows of %.0f ms (found %.2f s in)'
              % (alignment['score'], alignment['windows'],
                 alignment['window_ms'], alignment['lag_s']))
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
    parser.add_argument('--pic16-clock-ppm', '--slave-clock-ppm', type=int, default=0,
                        help='initial PIC16 oscillator frequency offset in ppm '
                             '(e.g. +15000 for +1.5%%, -18000 for -1.8%%)')
    parser.add_argument('--pic16-drift', '--pic16-drift-profile', type=str, default=None,
                        help='dynamic thermal/drift profile (e.g. "thermal", "linear", '
                             '"realistic", "stress", or "thermal_max=3000,tau=2000,linear=20")')

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
            if not movie.has_regions():
                # The controller checks every frame's device byte against
                # the id it is strapped to and drops the frame when they
                # disagree -- it never reaches the audio, so the recording
                # is the module's power-on state and nothing else. Cheap to
                # do by accident, and expensive to mistake for a firmware
                # fault, so say it here rather than leave it to the console.
                print('  none of this movie\'s regions are for device %d; '
                      'it is a device %s movie. The controller will reject '
                      'every frame and stream no audio -- pass --device-id.'
                      % (args.device_id,
                         '/'.join(str(d) for d in sorted(
                             {r['device'] for r in movie.regions
                              if r['type'] != 2}))),
                      file=sys.stderr)

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
