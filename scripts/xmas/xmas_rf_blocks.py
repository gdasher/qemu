#!/usr/bin/env python3
"""GNU Radio blocks that turn the emulated radio's own outputs into a signal.

The PIC16 in the XMAS transmitter does not produce a waveform anyone can
sample: it produces a *number*. Once per audio sample its interrupt writes a
new 20-bit increment into NCO1, and the NCO turns that into a square wave at
`inc * 32 MHz / 2**20` -- the intermediate frequency. A separate ADF4002
supplies the local oscillator, and the two are mixed on the board to reach the
carrier.

So the emulator does not have to model an oscillator to be honest about what
the radio is transmitting. It only has to say what the increment was and when
it changed, which is what `-M pic16-devboard,rf-dump=<file>` writes:

    <ns> nco inc=<hex5>
    <ns> lo hz=<n> r=<n> n=<n> func=<hex6> locked=<0|1>

`nco_freq_source` reads that file and produces the instantaneous IF as a float
stream in hertz, which `blocks.vco_f` turns into the sine the board's mixer
would have seen. Because every increment carries the virtual time it was
written at, the stream keeps the firmware's own timing: an interrupt that ran
late, or a queue that ran dry and held the last increment, is in the audio
exactly as it would be on air.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import numpy as np
from gnuradio import gr

# The NCO's clock: HFINTOSC at 32 MHz, which is also what TMR0 counts (see
# PROTOCOL.md in the XMASNGFMv2 firmware). The increment is 20 bits.
NCO_CLOCK_HZ = 32_000_000
NCO_BITS = 20


class RfDump:
    """An rf-dump file, read into arrays.

    `times` is virtual nanoseconds and `freqs` the IF in hertz at each of them,
    both in file order, which is time order. `lo_events` is every ADF4002 latch
    as (time_ns, dict).
    """

    def __init__(self, path):
        times = []
        incs = []
        self.lo_events = []

        with open(path, 'r', errors='replace') as f:
            for line in f:
                fields = line.split()
                if len(fields) < 2:
                    continue
                try:
                    when = int(fields[0])
                except ValueError:
                    continue
                if fields[1] == 'nco':
                    for token in fields[2:]:
                        if token.startswith('inc='):
                            times.append(when)
                            incs.append(int(token[4:], 16))
                            break
                elif fields[1] == 'lo':
                    kv = {}
                    for token in fields[2:]:
                        if '=' in token:
                            key, value = token.split('=', 1)
                            kv[key] = int(value, 16 if key == 'func' else 10)
                    self.lo_events.append((when, kv))

        self.times = np.asarray(times, dtype=np.int64)
        self.increments = np.asarray(incs, dtype=np.int64)
        self.freqs = self.increments.astype(np.float64) * NCO_CLOCK_HZ / (1 << NCO_BITS)

    def __len__(self):
        return len(self.times)

    def duration_ns(self):
        return int(self.times[-1] - self.times[0]) if len(self.times) > 1 else 0

    def lo_hz(self):
        """The local oscillator the transmitter settled on, or None.

        A movie retunes at most once, at its start, so a run with more than one
        distinct LO is worth saying out loud rather than averaging away.
        """
        seen = [kv['hz'] for _, kv in self.lo_events if 'hz' in kv]
        return seen[-1] if seen else None

    def lo_changes(self):
        seen = [kv['hz'] for _, kv in self.lo_events if 'hz' in kv]
        return len(set(seen))

    def base_frequency(self):
        """A guess at the unmodulated IF, for a run whose carrier is not known.

        The median rather than the mean or the mode: modulation is symmetric
        about the resting increment but spends longest at its turning points,
        so the commonest increment is an extreme rather than the centre, and a
        long silence would drag the mean nowhere useful. Prefer
        `if_frequency()` whenever the carrier is known -- this is only for a
        dump read on its own.
        """
        if not len(self.increments):
            return 0.0
        return float(np.median(self.increments)) * NCO_CLOCK_HZ / (1 << NCO_BITS)

    def if_frequency(self, carrier_hz):
        """Exactly where the transmitter rests, given the carrier it was told.

        The firmware works out `IF = LO - carrier` and then rounds it into a
        20-bit increment, so this is that same arithmetic: the receiver tunes
        to the increment the transmitter really writes, rounding and all,
        rather than to the frequency it was aiming at.
        """
        lo = self.lo_hz()
        if not lo or not carrier_hz:
            return None
        inc = ((lo - carrier_hz) * (1 << NCO_BITS)) // NCO_CLOCK_HZ
        return float(inc) * NCO_CLOCK_HZ / (1 << NCO_BITS)

    def gaps_ns(self, factor=4.0):
        """Intervals between increments far longer than the usual one.

        The firmware rewrites the increment every sample period, so a long gap
        is the sample interrupt not running -- the machine stopped, or the
        firmware was busy elsewhere. It sounds like a dropout, and it is one.
        """
        if len(self.times) < 3:
            return []
        deltas = np.diff(self.times)
        typical = float(np.median(deltas))
        if typical <= 0:
            return []
        at = np.nonzero(deltas > typical * factor)[0]
        return [(int(self.times[i]), int(deltas[i])) for i in at]


class nco_freq_source(gr.sync_block):
    """The transmitter's instantaneous IF, in hertz, at the graph's rate.

    `offset_hz` is subtracted from every sample. The real IF is around 10 MHz
    and the real carrier around 98 MHz; neither can be sampled at any rate a
    simulation can afford. Subtracting a constant from the IF *and* from the
    local oscillator leaves the difference between them -- which is the whole
    of the modulation -- untouched, so the receiver downstream sees exactly the
    signal the board's mixer produces, only lower down the spectrum.
    """

    def __init__(self, dump, samp_rate, offset_hz=0.0):
        gr.sync_block.__init__(self, name='nco_freq_source',
                               in_sig=None, out_sig=[np.float32])
        if not len(dump):
            raise ValueError('the rf-dump holds no NCO increments: the '
                             'transmitter never ran its sample interrupt')
        self.dump = dump
        self.samp_rate = float(samp_rate)
        self.offset_hz = float(offset_hz)
        self.start_ns = int(dump.times[0])
        self.total = int(dump.duration_ns() * self.samp_rate / 1e9)
        self.produced = 0

    def work(self, input_items, output_items):
        out = output_items[0]
        n = min(len(out), self.total - self.produced)
        if n <= 0:
            return -1        # the dump has run out; the run is over

        k = self.produced + np.arange(n, dtype=np.int64)
        when = self.start_ns + (k * 1_000_000_000 // int(self.samp_rate))
        # Each increment holds until the next one is written, which is what
        # `right` minus one finds. Clamped, so nothing runs off either end.
        at = np.searchsorted(self.dump.times, when, side='right') - 1
        np.clip(at, 0, len(self.dump.times) - 1, out=at)

        out[:n] = (self.dump.freqs[at] - self.offset_hz).astype(np.float32)
        self.produced += n
        return n


class nco_if_source(gr.sync_block):
    """The same source, named a file rather than a dump.

    This is the shape a flowgraph wants: everything it needs is a parameter
    with a default, the file is opened when the graph runs rather than when
    the graph is built, and where to put the IF is worked out here instead of
    in a chain of variables. `xmas_superhet.grc` uses this one;
    `xmas_superhet.py` uses `nco_freq_source` directly, having already read
    the dump to report on it.

    A dump that cannot be read leaves the block silent rather than stopping
    the graph from being built, which is what lets Companion open the
    flowgraph before there is a run to look at.
    """

    def __init__(self, rf_dump='rf.txt', samp_rate=1000000.0,
                 carrier_hz=98000000, if_sim_hz=172050.0):
        gr.sync_block.__init__(self, name='nco_if_source',
                               in_sig=None, out_sig=[np.float32])
        self.samp_rate = float(samp_rate)
        self.inner = None
        try:
            dump = RfDump(rf_dump)
        except OSError:
            return
        if not len(dump):
            return
        centre = dump.if_frequency(carrier_hz) or dump.base_frequency()
        self.inner = nco_freq_source(dump, samp_rate, centre - float(if_sim_hz))

    def work(self, input_items, output_items):
        if self.inner is None:
            output_items[0][:] = 0.0
            return len(output_items[0])
        return self.inner.work(input_items, output_items)
