#!/usr/bin/env python3
"""Stand in for the PIC32 on the FM co-simulation link, and time the slave.

The link (hw/pic16/pic16_cosim_link.c) makes the master the clock: every
message carries the virtual nanosecond it happens at, and the PIC16 never runs
past the latest moment it has been told about. So a script at this end can
clock a byte at any instant it likes and read back exactly what the firmware
really had in SSP1BUF when it arrived -- which is what makes the slave's
receive path measurable without instrumenting it.

Two measurements, and both answer questions xmas_sim.py cannot:

    fm_probe.py budget
        The smallest byte gap the module can be fed at without losing a byte.
        Flood a long chip-select frame with CMD_NOP -- the cheapest byte in
        the protocol -- and bisect the gap until CMD_GET_STATUS stops
        reporting FM_STATUS_SPI_OVERRUN. Run at two sample rates and the two
        results separate the per-byte cost from the sample interrupt's share
        of the same core, because the interrupt's share scales with the rate
        and the per-byte cost does not:

            gap = per_byte / (1 - isr / period)

    fm_probe.py stream --rate 22050 --bits 16 --gap 20900
        Sustained streaming at a chosen depth and wire pace: per frame, how
        many reply slots the module managed to load, what CMD_GET_STATUS
        latched, and where the queue level lines sat. This is the master's
        streaming behaviour with the master's own pacing loop taken out, so
        an answer here is about the module alone.

The costs come out in nanoseconds; divided by 125 ns they are instruction
cycles of the PIC16F15355's 8 MIPS. QEMU charges one icount step per
instruction at shift 7, so a branch-heavy path is if anything cheaper here
than on the real core.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import math
import os
import socket
import subprocess
import sys
import tempfile
import time

FM_LINK_VERSION = 2

CMD_NOP = 0x00
CMD_SET_CARRIER = 0x01
CMD_SET_DEVIATION = 0x02
CMD_SET_ATTENUATION = 0x03
CMD_SET_SAMPLE_RATE = 0x04
CMD_SET_MODE = 0x05
CMD_AUDIO_DATA = 0x06
CMD_GET_STATUS = 0x07

MODE_SILENCE = 0
MODE_FM_AUDIO = 1

RESP_NAMES = {0x00: 'READY', 0x01: 'MORE', 0x02: 'OK', 0x03: 'OVERFLOW',
              0x04: 'FAULT', 0x06: 'APPLYING', 0x0E: 'ERROR', 0xFF: 'EMPTY'}

STATUS_ISR_OVERRUN = 0x01
STATUS_SPI_OVERRUN = 0x02
STATUS_UNDERRUN = 0x04

# The sample rates the module accepts, as SET_SAMPLE_RATE's three big-endian
# bytes, and what its TMR0 divider really lands on (kSampleRates in the
# firmware): the interrupt's period is the actual rate, not the asked one.
RATES = {
    16000: (0x00, 0x3E, 0x80, 16000.0),
    22050: (0x00, 0x56, 0x22, 22038.6),
    32000: (0x00, 0x7D, 0x00, 32000.0),
    44100: (0x00, 0xAC, 0x44, 44077.1),
    48000: (0x00, 0xBB, 0x80, 48048.0),
}

# What the XMASNg controller leaves between bytes while it configures:
# a quarter of the stream's wire rate (FM_CONFIG_BAUD_DIV in its fm_master.h).
CONFIG_GAP_NS = 41000

# 8 MIPS: one instruction cycle.
INSTRUCTION_NS = 125.0


class Link:
    """One PIC16 under this script's clock."""

    def __init__(self, qemu, firmware, workdir, icount_shift=7):
        self.dir = workdir
        self.sock_path = os.path.join(workdir, 'fm.sock')
        self.rf_path = os.path.join(workdir, 'rf.txt')
        open(self.rf_path, 'w').close()
        argv = [
            qemu,
            '-M', ','.join(['pic16-devboard', 'soc=pic16f15355',
                            'adf4002=RC1:RC2:RA6', 'fm-link=fmlink',
                            'rf-dump=' + self.rf_path]),
            '-bios', firmware,
            '-display', 'none', '-nodefaults', '-monitor', 'none',
            '-chardev', 'socket,id=fmlink,path=%s,server=on,wait=on'
                        % self.sock_path,
            '-icount', 'shift=%d' % icount_shift,
            '-d', 'guest_errors', '-D', os.path.join(workdir, 'pic16.log'),
        ]
        self.proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL)
        deadline = time.time() + 30
        while not os.path.exists(self.sock_path):
            if self.proc.poll() is not None or time.time() > deadline:
                raise RuntimeError('the transmitter did not open its link '
                                   'socket')
            time.sleep(0.05)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.sock_path)
        self.sock.settimeout(300)
        self.buf = b''
        self.now = 0

        hello = self._line()
        if not hello.startswith('HELLO '):
            raise RuntimeError('no greeting from the link: %r' % hello)
        self._send('HELLO fm-link %d' % FM_LINK_VERSION)

    # -- the wire ---------------------------------------------------------
    def _line(self):
        while b'\n' not in self.buf:
            more = self.sock.recv(65536)
            if not more:
                raise RuntimeError('the link closed')
            self.buf += more
        line, self.buf = self.buf.split(b'\n', 1)
        return line.decode().strip()

    def _send(self, msg):
        self.sock.sendall((msg + '\n').encode())

    def byte(self, value):
        """Clock one byte at the cursor; returns what came back."""
        self._send('X %d %02X' % (self.now, value))
        while True:
            reply = self._line()
            if reply.startswith('R '):
                return int(reply.split()[1], 16)

    def select(self, asserted):
        self._send('C %d %d' % (self.now, 1 if asserted else 0))

    def idle(self, ns):
        """Let the guest run for `ns` with the wire quiet."""
        self.now += ns
        self._send('T %d' % self.now)

    def level(self):
        """The queue level band the module is driving on its LVL lines."""
        self._send('P %d' % self.now)
        while True:
            reply = self._line()
            if reply.startswith('L '):
                return int(reply.split()[1], 16)

    def frame(self, data, gap_ns):
        """One chip-select transaction, `gap_ns` between bytes."""
        self.select(True)
        self.now += gap_ns
        rx = []
        for value in data:
            rx.append(self.byte(value))
            self.now += gap_ns
        self.select(False)
        self.now += gap_ns
        return rx

    def close(self):
        try:
            self._send('Q %d' % self.now)
            time.sleep(0.2)
        except OSError:
            pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


def configure(link, rate=22050, mode=MODE_FM_AUDIO, boot_ns=400_000_000):
    """Bring the module up the way the controller does, at its pacing.

    98 MHz, 75 kHz deviation, no attenuation. The two staged settings get a
    few milliseconds to finish their slices, and SET_SAMPLE_RATE is retried
    because its answer can miss its slot behind a slice -- which is the
    master's own behaviour (FM_CMD_RETRIES).
    """
    link.idle(boot_ns)
    link.frame([CMD_SET_MODE, MODE_SILENCE, CMD_NOP], CONFIG_GAP_NS)
    link.frame([CMD_SET_CARRIER, 0x04, 0x38, 0x05, 0x00, 0x00, CMD_NOP],
               CONFIG_GAP_NS)
    link.idle(5_000_000)
    link.frame([CMD_SET_DEVIATION, 0x09, 0x99, CMD_NOP], CONFIG_GAP_NS)
    link.idle(5_000_000)
    link.frame([CMD_SET_ATTENUATION, 0x00, CMD_NOP], CONFIG_GAP_NS)
    for _ in range(4):
        rx = link.frame([CMD_SET_SAMPLE_RATE] + list(RATES[rate][:3])
                        + [CMD_NOP], CONFIG_GAP_NS)
        if rx[-1] == 0x02:
            break
    else:
        raise RuntimeError('the module would not take %d Hz' % rate)
    link.frame([CMD_SET_MODE, mode, CMD_NOP], CONFIG_GAP_NS)


def read_status(link):
    """CMD_GET_STATUS, which reads and clears the latched flags.

    The answer is a raw FM_STATUS_* bitmask, so it shares its values with the
    response codes -- 0x02 here is SPI_OVERRUN, not OK.
    """
    return link.frame([CMD_GET_STATUS, CMD_NOP, CMD_NOP], CONFIG_GAP_NS)[1]


def status_names(value):
    names = [name for bit, name in ((STATUS_ISR_OVERRUN, 'isr-overrun'),
                                    (STATUS_SPI_OVERRUN, 'spi-overrun'),
                                    (STATUS_UNDERRUN, 'underrun'))
             if value & bit]
    return ', '.join(names) or 'clean'


def audio_frame(nsamples, bits, phase, rate, freq=1000.0, amplitude=0.8):
    """A frame's audio as the master sends it: 64-sample packets, back to
    back under one chip select, carrying a sine so the NCO dump can be read
    back afterwards."""
    out = []
    sent = 0
    while sent < nsamples:
        count = min(64, nsamples - sent)
        out += [CMD_AUDIO_DATA, bits, count]
        for _ in range(count):
            value = int(amplitude * 32767 *
                        math.sin(2 * math.pi * freq * phase / rate))
            phase += 1
            if bits == 16:
                word = value & 0xFFFF
                out += [word >> 8, word & 0xFF]
            else:
                out.append((value >> 8) & 0xFF)
        sent += count
    return out, phase


class Probe:
    """A fresh module per measurement: a latched fault outlives a frame."""

    def __init__(self, args):
        self.args = args
        self.keep = args.workdir

    def _session(self, rate, mode=MODE_FM_AUDIO):
        work = self.keep or tempfile.mkdtemp(prefix='fm-probe-')
        os.makedirs(work, exist_ok=True)
        link = Link(self.args.qemu, self.args.firmware, work,
                    self.args.icount_shift)
        configure(link, rate, mode)
        read_status(link)          # whatever the boot left is not the test's
        return link, work

    def loses_bytes(self, rate, gap_ns, nbytes, frames=3):
        """Does a long frame at this pace cost the module a byte?"""
        link, _ = self._session(rate)
        try:
            for _ in range(frames):
                link.frame([CMD_NOP] * nbytes, gap_ns)
                if read_status(link) & STATUS_SPI_OVERRUN:
                    return True
                link.idle(40_000_000)
            return False
        finally:
            link.close()

    def min_gap(self, rate, nbytes=1200, lo=10_000, hi=60_000,
                resolution=250):
        """The smallest byte gap this module survives, by bisection."""
        while hi - lo > resolution:
            mid = (lo + hi) // 2
            if self.loses_bytes(rate, mid, nbytes):
                lo = mid
            else:
                hi = mid
        return hi


def cmd_budget(args):
    probe = Probe(args)
    rates = args.rates or [16000, 48000]
    gaps = {}
    for rate in rates:
        gaps[rate] = probe.min_gap(rate, nbytes=args.bytes)
        print('  %5d Hz: the module needs %6.2f us a byte (%5.1f instructions)'
              % (rate, gaps[rate] / 1000.0, gaps[rate] / INSTRUCTION_NS),
              flush=True)

    if len(rates) >= 2:
        # gap = per_byte / (1 - isr / period), at two rates.
        (r_a, r_b) = rates[0], rates[-1]
        g_a, g_b = gaps[r_a] / 1000.0, gaps[r_b] / 1000.0
        t_a = 1e6 / RATES[r_a][3]
        t_b = 1e6 / RATES[r_b][3]
        denom = (g_b / t_b) - (g_a / t_a)
        if abs(denom) > 1e-9:
            isr = (g_b - g_a) / denom
            per_byte = g_a * (1.0 - isr / t_a)
            print()
            print('  receive and parse: %5.2f us a byte  (%5.1f instructions)'
                  % (per_byte, per_byte * 1000 / INSTRUCTION_NS))
            print('  the sample interrupt: %5.2f us  (%5.1f instructions)'
                  % (isr, isr * 1000 / INSTRUCTION_NS))
            print()
            print('  what fits a %d ms frame at that cost:' % args.frame_ms)
            print('    %-6s %-5s %10s %10s %8s'
                  % ('rate', 'bits', 'frame has', 'can parse', ''))
            for rate in sorted(RATES):
                if rate not in gaps:
                    continue
                capacity = args.frame_ms * 1000.0 / (gaps[rate] / 1000.0)
                for bits in (8, 16):
                    need = frame_bytes(rate, bits, args.frame_ms)
                    print('    %-6d %-5d %10d %10d %7.2fx %s'
                          % (rate, bits, need, capacity, need / capacity,
                             'fits' if need <= capacity else 'OVER'))
    return 0


def frame_bytes(rate, bits, frame_ms):
    """The bytes one frame of audio takes on the wire: the samples at their
    depth, plus a three-byte header every 64 of them."""
    samples = int(round(RATES[rate][3] * frame_ms / 1000.0))
    packets = -(-samples // 64)
    return samples * (2 if bits == 16 else 1) + 3 * packets


def cmd_stream(args):
    probe = Probe(args)
    link, work = probe._session(args.rate)
    samples = int(round(RATES[args.rate][3] * args.frame_ms / 1000.0))
    frame_ns = args.frame_ms * 1_000_000
    gap = args.gap or int(round(frame_ns / frame_bytes(args.rate, args.bits,
                                                       args.frame_ms)))
    print('%d Hz, %d-bit, %d samples a frame, %d bytes, %.2f us a byte'
          % (args.rate, args.bits, samples,
             frame_bytes(args.rate, args.bits, args.frame_ms), gap / 1000.0))
    phase = 0
    try:
        for index in range(args.frames):
            started = link.now
            data, phase = audio_frame(samples, args.bits, phase,
                                      RATES[args.rate][3])
            rx = link.frame(data, gap)
            loaded = sum(1 for value in rx if value != 0xFF)
            band = link.level()
            status = read_status(link)
            spare = frame_ns - (link.now - started)
            print('  frame %2d: %4d of %4d reply slots loaded, level band %d, '
                  '%s%s'
                  % (index, loaded, len(rx), band, status_names(status),
                     '' if spare >= 0
                     else ', OVERRUNS THE FRAME BY %.1f ms' % (-spare / 1e6)),
                  flush=True)
            if spare > 0:
                link.idle(spare)
    finally:
        link.close()
    print()
    print("the run's NCO increments are in %s"
          % os.path.join(work, 'rf.txt'))
    return 0


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    build = os.path.abspath(os.path.join(here, '..', '..', 'build'))

    parser = argparse.ArgumentParser(
        description=__doc__.split('\n')[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    parser.add_argument('mode', choices=('budget', 'stream'))
    parser.add_argument('--qemu',
                        default=os.path.join(build, 'qemu-system-pic16'))
    parser.add_argument('--firmware',
                        default=os.path.expanduser(
                            '~/git/XMASNGFMv2/build/default.hex'),
                        help='the .hex to measure; a CI artifact makes this '
                             'an A/B against any earlier revision')
    parser.add_argument('--icount-shift', type=int, default=7,
                        help="7 is the PIC16's real 8 MIPS")
    parser.add_argument('--workdir', help='put the run\'s files here and '
                                          'keep them')
    parser.add_argument('--frame-ms', type=int, default=40,
                        help="the movie's frame period")

    parser.add_argument('--rates', type=int, nargs='+',
                        help='budget: which sample rates to measure '
                             '(two or more separate the interrupt out)')
    parser.add_argument('--bytes', type=int, default=1200,
                        help='budget: how long a frame to flood')

    parser.add_argument('--rate', type=int, default=22050,
                        choices=sorted(RATES))
    parser.add_argument('--bits', type=int, default=16, choices=(8, 16))
    parser.add_argument('--gap', type=int,
                        help="stream: ns between bytes (default: the pace "
                             "that just fills the frame)")
    parser.add_argument('--frames', type=int, default=12)
    args = parser.parse_args()

    if args.rates:
        for rate in args.rates:
            if rate not in RATES:
                parser.error('%d is not a rate the module accepts' % rate)

    if not os.path.exists(args.firmware):
        parser.error('no firmware at %s' % args.firmware)

    return cmd_budget(args) if args.mode == 'budget' else cmd_stream(args)


if __name__ == '__main__':
    sys.exit(main())
