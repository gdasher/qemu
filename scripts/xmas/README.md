# The XMAS audio path, end to end

Play a movie through both of the product's microcontrollers and record what a
radio tuned to it would have heard.

    scripts/xmas/xmas_sim.py --movie ~/movies/5 --out radio.wav

The XMAS transmitter is two boards. The PIC32MK reads a movie off an SD card,
drives eight WS2812 strings, and streams the movie's audio over SPI3; the
PIC16F15355 on the other end of that link turns the stream into an FM signal
and mixes it up to the carrier. Each half has been emulated on its own for a
while — the PIC32 against `hw/chips/fm_transmitter.c`, a model of the
transmitter, and the PIC16 against a Python script standing in for the PIC32.
This runs the two real firmwares against each other and demodulates the
result, so "does the movie come out of the radio sounding right" becomes a
question with an answer.

## Why two QEMUs

There is no machine holding both chips because there is no QEMU binary holding
both instruction sets: PIC16 and MIPS are separate targets under
`configs/targets/`, built separately, and QEMU has no heterogeneous machine.
So `xmas_sim.py` starts one of each and joins them with a link that carries
SPI bytes and the virtual time they were clocked at.

    qemu-system-mipsel -M pic32mk-devboard,...,fm-link=<chardev>
              |  hw/chips/fm_link.c            X <t_ns> <byte>
              |                          <------------------------>
              |  hw/pic16/pic16_cosim_link.c   R <byte>
    qemu-system-pic16  -M pic16-devboard,...,fm-link=<chardev>,rf-dump=<file>

The PIC32 is the clock master. It stamps every byte and every heartbeat with
its own virtual clock, and the PIC16 never runs past the last moment it has
been told about — so the two guests advance together, a byte reaches the
transmitter's firmware at the same virtual instant it left the controller, and
the reply is whatever that firmware really had in SSP1BUF when it did.

That matters because it is the only way the pacing means anything. The
transmitter drains its sample queue on its own timer; the controller fills it
from its own frame clock. If the two clocks drifted, every run would show
underruns that were an artefact of the harness. Instead, `-icount` on both
sides and the link between them make a run a deterministic function of the two
guests' execution.

SPI3 is already timed on the wire (`hw/pic32/pic32mk_soc.c`, "on the header
sits the FM transmitter's own microcontroller, which has to keep up byte by
byte"), so the bytes are spaced by the time they really take and the
transmitter sees the gaps the silicon would.

## Why the radio is a file

The PIC16 does not produce a waveform. Once per audio sample its interrupt
writes a 20-bit increment into NCO1, and the NCO turns that into a square wave
at `inc × 32 MHz / 2²⁰` — the intermediate frequency. An ADF4002 supplies the
local oscillator and the board mixes the two. So the emulator does not have to
model an oscillator to be honest about what is being transmitted; it only has
to say what the increment was and when it changed, which is what
`-M pic16-devboard,rf-dump=<file>` writes:

    <ns> nco inc=<hex5>
    <ns> lo hz=<n> r=<n> n=<n> func=<hex6> locked=<0|1>

A file rather than a `pic16-sim-bridge` event, because there is one of these
per audio sample and the bridge is lock-step: a round trip each would cost
more than the emulation.

## The receiver

`xmas_superhet.py` builds what the board's other half would be — two sine
waves, a mixer and an FM detector:

    nco_freq_source ──▶ vco_f ─────────┐          the IF, from rf-dump
                                        ×  mixer
    sig_source_f (the LO) ─────────────┘
                                        │
                    freq_xlating_fir_filter_fcf   take the difference product,
                                        │         reject the sum
                       quadrature_demod_cf        FM detector
                                        │
                          multiply_const_ff       hertz to full scale
                                        │
                            fir_filter_fff        down to the audio rate
                                        │
                             wavfile_sink [, audio.sink]

Everything runs at an offset. A real superhet here has its LO near 108 MHz and
its IF near 10 MHz, and sampling either would cost more than the emulation that
produced them. The LO and the IF are both shifted down by the same constant,
which leaves the difference between them — the carrier, and every hertz of
deviation on it — exactly as the hardware makes it. The numbers in the graph
are not the numbers on the board; the audio is.

The rates are chosen from the movie's audio rate and the deviation in use, so
every decimation is a whole number and the baseband comfortably clears
Carson's rule. At 22.05 kHz and ±75 kHz that is 1.54 MHz sampled, the IF at
172 kHz, the LO at 344 kHz and the mixer's difference product at 172 kHz.

The detector's gain is negative on purpose: the board injects on the high side
(LO = carrier + IF), so its output is `carrier − k·audio` and the spectrum is
inverted. A real receiver inverts it back, and so does this one.

`xmas_superhet.grc` is the same graph for `gnuradio-companion`, if you would
rather look at it than read it.

## Files

| | |
|---|---|
| `xmas_sim.py` | the driver: builds the card, runs both machines, decodes the link, demodulates, reports |
| `xmas_superhet.py` | the receiver, and the frequency plan it picks |
| `xmas_rf_blocks.py` | `RfDump` and the `nco_freq_source` block |
| `xmas_superhet.grc` | the receiver as a GNU Radio Companion flowgraph |

## Running it

    scripts/xmas/xmas_sim.py --movie ~/movies/5 --out radio.wav

Useful flags:

    --loops N            play the movie through N times (default 1)
    --frames N           stop after N latched LED frames instead
    --device-id N        what the board is strapped to; a movie's regions name
                         the device they are for, and a mismatch plays nothing
    --listen             play the result through the host as well as writing it
    --workdir DIR        keep the run's files (link dump, RF dump, LED dump,
                         console, both QEMU logs) instead of using a temp dir
    --rf-only DIR        demodulate a previous run again without emulating it
    --pic16-icount-shift N
                         1 instruction is 2^N ns. 7 is the PIC16's real 8 MIPS
                         and the default. A lower number gives its core more
                         headroom than its silicon has, which is a way to ask
                         "would this work if the firmware were faster?" — the
                         radio's own timing is unaffected either way, because
                         TMR0, the NCO and the PLL are clocked in virtual time

The report at the end covers the link (what was configured, how many packets
and samples crossed it, the queue level, any fault the transmitter latched),
the radio (the LO, the IF, gaps in the sample interrupt, the receiver's plan)
and the run (frames played, and how well the recording correlates with the
movie's own `audio.wav`).

## What it found

The first run of this pair found a bug that had been in the product since the
transmitter was written, and that neither side's tests could see.

The controller set SPI3's baud from the audio rate and sent its configuration
at that rate too — a byte every 21 µs at 22.05 kHz and 16 bits. The
transmitter cannot answer in that. Measured in the `pic16-devboard` machine at
its real 8 MIPS: accumulating one byte of a 32-bit parameter takes about 17 µs
and validating a command's last byte another 6, and *applying* a setting is an
order of magnitude more again — 398 µs to program the PLL, 605 µs to rebuild
the deviation tables, 145 µs to retime the sample interrupt. A module that has
not reloaded `SSP1BUF` leaves 0xFF on the wire, and 0xFF is `FM_RESP_ERROR`,
so the controller read "still working" as "refused", gave up, and played every
movie silently.

It went unseen because each side was only ever tested against a model of the
other, and both models were calibrated for the audio path — where the pacing
is right by design — rather than for the configuration around it.

The fix is on both sides, and `hw/chips/fm_transmitter.c` now carries the
measured cost of applying each setting so `moviecheck.py` catches it without
the co-simulation.

## Notes

- Start the PIC16 first; it listens and waits, and the PIC32 connects. The
  driver does this for you.
- The RF dump is about 25 bytes per audio sample — 73 MB for a 66-second movie
  at 22.05 kHz. A FIFO works if you would rather not keep it.
- The link is one round trip per SPI byte, which for a 66-second movie at
  22.05 kHz and 16 bits is about 3.3 million of them. That is the price of
  lock-step and it is worth it: it is what makes an underrun in the recording
  an underrun the firmware really had.
