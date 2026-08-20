#!/usr/bin/env python3
"""A superheterodyne receiver for the emulated XMAS transmitter.

The transmitter is two oscillators and a mixer: an NCO at an intermediate
frequency that carries the modulation, and an ADF4002 local oscillator that
moves it up to the carrier. This is the other half of that -- the two sine
waves, the mixer, and an FM demodulator -- so what comes out is what a radio
tuned to the board would hear.

    nco_freq_source ---> vco_f -------.
      (the IF, in Hz, from rf-dump)    |
                                       *  mixer
    sig_source_f (the LO) -------------'
                                       |
                       freq_xlating_fir_filter_fcf   pick the difference
                                       |             product, reject the sum
                          quadrature_demod_cf        FM detector
                                       |
                              multiply_const_ff      hertz to full scale
                                       |
                            fir_filter_fff           down to the audio rate
                                       |
                     single_pole_iir_filter_ff       75 us de-emphasis
                                       |
                            wavfile_sink [, audio.sink]

Everything runs at an offset. A real superhet here has its LO near 108 MHz and
its IF near 10 MHz; sampling either would cost more than the emulation that
produced them. The LO and the IF are both shifted down by the same constant,
which leaves the difference between them -- the carrier, and every hertz of
deviation on it -- exactly as the hardware makes it. So the numbers in the
graph are not the numbers on the board, and the audio is.

The demodulator's gain is negative on purpose. The board injects on the high
side (LO = carrier + IF), so its output is `carrier - k*audio`: the spectrum is
inverted, and a receiver has to invert it back. That is true of the real board
too.

The de-emphasis is the other half of the broadcast standard. The master
pre-emphasizes what it streams -- the 75 us US filter, y[n] = x[n] -
alpha*x[n-1] with alpha = exp(-1/(rate*tau)), FM_Preemph in the controller's
fm_master.h -- and every receiver undoes it with the matching low-pass. This
one runs the exact inverse at the audio rate, 1/(1 - alpha*z^-1) as a single
real pole with its gain matched so the cascade is unity, and the recording is
the movie's audio back rather than its treble-boosted transmit form.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import math

from gnuradio import analog, blocks, filter, gr
from gnuradio.filter import firdes

from xmas_rf_blocks import nco_freq_source


# The broadcast de-emphasis time constant: 75 us in North America.
DEEMPH_TAU_S = 75e-6


class Plan:
    """Where the receiver puts everything, and at what rates.

    Chosen from the deviation and the audio rate rather than fixed, so a movie
    at any of the transmitter's rates gets a graph whose decimations are whole
    numbers and whose bandwidths clear Carson's rule.
    """

    def __init__(self, audio_rate, deviation_hz, deemph_tau_s=DEEMPH_TAU_S):
        self.audio_rate = int(audio_rate)
        self.deviation_hz = float(deviation_hz)
        self.deemph_tau_s = float(deemph_tau_s)
        # The pole of the master's pre-emphasis, which the receiver's
        # de-emphasis mirrors.
        self.deemph_alpha = math.exp(-1.0 /
                                     (self.audio_rate * self.deemph_tau_s)) \
            if self.deemph_tau_s > 0 else 0.0

        # How far the signal strays from its centre: the peak deviation plus
        # the top of the audio band.
        self.guard = self.deviation_hz + self.audio_rate / 2.0

        # The IF sits at twice that, so the modulation never reaches zero; the
        # difference product lands at the same place, which puts the LO at
        # twice the IF.
        self.if_hz = 2.0 * self.guard
        self.rf_hz = 2.0 * self.guard
        self.lo_hz = self.if_hz + self.rf_hz

        # Baseband: Carson's bandwidth is 2*guard, with a quarter over for the
        # filter's skirts, rounded up to a whole number of audio samples.
        self.audio_decim = max(1, math.ceil(2.5 * self.guard / self.audio_rate))
        self.bb_rate = self.audio_decim * self.audio_rate

        # The graph's own rate has to hold the mixer's sum product, which
        # reaches lo + if + guard, with room for the anti-alias skirt.
        rf_min = 2.2 * (self.lo_hz + self.if_hz + self.guard)
        self.bb_decim = max(1, math.ceil(rf_min / self.bb_rate))
        self.samp_rate = self.bb_decim * self.bb_rate

    def describe(self):
        return ('%.3f MHz sampled, IF %.1f kHz, LO %.1f kHz, product %.1f kHz, '
                'baseband %.1f kHz (/%d), audio %d Hz (/%d), de-emphasis %s'
                % (self.samp_rate / 1e6, self.if_hz / 1e3, self.lo_hz / 1e3,
                   self.rf_hz / 1e3, self.bb_rate / 1e3, self.bb_decim,
                   self.audio_rate, self.audio_decim,
                   '%.0f us' % (self.deemph_tau_s * 1e6)
                   if self.deemph_alpha else 'off'))


class XmasSuperhet(gr.top_block):
    def __init__(self, dump, plan, wav_path=None, listen=False,
                 if_centre_hz=None):
        gr.top_block.__init__(self, 'XMAS superhet receiver')
        self.plan = plan

        # Where the transmitter's unmodulated IF really is, so the receiver
        # tunes to the transmitter rather than to the arithmetic.
        centre = dump.base_frequency() if if_centre_hz is None else if_centre_hz
        offset = centre - plan.if_hz

        self.source = nco_freq_source(dump, plan.samp_rate, offset)
        self.if_sine = blocks.vco_f(plan.samp_rate, 2 * math.pi, 1.0)
        self.lo_sine = analog.sig_source_f(plan.samp_rate, analog.GR_COS_WAVE,
                                           plan.lo_hz, 1.0)
        self.mixer = blocks.multiply_ff()

        # The mixer leaves the difference product at rf_hz and the sum an
        # octave up. This takes the first and throws away the second, and
        # brings what is left down to a complex baseband the detector can read.
        taps = firdes.low_pass(1.0, plan.samp_rate, plan.guard * 1.1,
                               plan.guard * 0.5)
        self.channel = filter.freq_xlating_fir_filter_fcf(
            plan.bb_decim, taps, plan.rf_hz, plan.samp_rate)

        # Angle per sample to hertz, then hertz to full scale -- negative,
        # because high-side injection inverts the spectrum on the way out.
        self.detector = analog.quadrature_demod_cf(plan.bb_rate / (2 * math.pi))
        self.scale = blocks.multiply_const_ff(-1.0 / plan.deviation_hz)

        audio_taps = firdes.low_pass(1.0, plan.bb_rate, plan.audio_rate * 0.45,
                                     plan.audio_rate * 0.15)
        self.audio_lpf = filter.fir_filter_fff(plan.audio_decim, audio_taps)

        # De-emphasis, the receiver's half of the broadcast standard: the
        # exact inverse of the master's y[n] = x[n] - alpha*x[n-1], run at
        # the audio rate. single_pole_iir is (1-alpha)/(1 - alpha*z^-1);
        # dividing the (1-alpha) back out leaves 1/(1 - alpha*z^-1), so the
        # cascade with the transmitter is unity and the recording sits at
        # the movie's own level.
        last = self.audio_lpf
        if plan.deemph_alpha:
            self.deemph = filter.single_pole_iir_filter_ff(
                1.0 - plan.deemph_alpha)
            self.deemph_gain = blocks.multiply_const_ff(
                1.0 / (1.0 - plan.deemph_alpha))
            last = self.deemph_gain

        self.connect(self.source, self.if_sine, (self.mixer, 0))
        self.connect(self.lo_sine, (self.mixer, 1))
        self.connect(self.mixer, self.channel, self.detector, self.scale,
                     self.audio_lpf)
        if plan.deemph_alpha:
            self.connect(self.audio_lpf, self.deemph, self.deemph_gain)

        self.sinks = []
        if wav_path:
            self.sinks.append(blocks.wavfile_sink(
                wav_path, 1, plan.audio_rate, blocks.FORMAT_WAV,
                blocks.FORMAT_PCM_16, False))
        if listen:
            from gnuradio import audio
            self.sinks.append(audio.sink(plan.audio_rate, '', True))
        if not self.sinks:
            self.sinks.append(blocks.null_sink(gr.sizeof_float))
        for sink in self.sinks:
            self.connect(last, sink)


def demodulate(dump, audio_rate, deviation_hz, wav_path=None, listen=False,
               carrier_hz=None):
    """Runs the receiver over a dump. Returns the plan it used."""
    plan = Plan(audio_rate, deviation_hz)
    top = XmasSuperhet(dump, plan, wav_path=wav_path, listen=listen,
                       if_centre_hz=dump.if_frequency(carrier_hz))
    top.start()
    top.wait()
    top.stop()
    return plan


def main():
    import argparse

    from xmas_rf_blocks import RfDump

    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('rf_dump', help='the file -M pic16-devboard,rf-dump= wrote')
    parser.add_argument('--out', help='write the demodulated audio here')
    parser.add_argument('--rate', type=int, default=22050,
                        help='audio sample rate of the movie (default 22050)')
    parser.add_argument('--deviation', type=int, default=75000,
                        help='peak deviation the transmitter was set to')
    parser.add_argument('--carrier', type=int,
                        help='carrier the transmitter was tuned to; without it '
                             'the receiver takes the median increment for the '
                             'resting frequency, which a movie with little '
                             'silence can pull off centre')
    parser.add_argument('--listen', action='store_true',
                        help='also play it through the host')
    args = parser.parse_args()

    dump = RfDump(args.rf_dump)
    plan = Plan(args.rate, args.deviation)
    print('%d NCO increments over %.3f s of guest time'
          % (len(dump), dump.duration_ns() / 1e9))
    print(plan.describe())
    demodulate(dump, args.rate, args.deviation, args.out, args.listen,
               carrier_hz=args.carrier)


if __name__ == '__main__':
    import os
    import sys

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
