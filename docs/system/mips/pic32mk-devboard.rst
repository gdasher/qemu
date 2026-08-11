Microchip PIC32MK development board (``pic32mk-devboard``)
==========================================================

A PIC32MK1024GPK100 and whatever chips you say are wired to it. The
microcontroller is a MIPS32 microAptiv MCU core running microMIPS, so it uses
``qemu-system-mipsel`` and the ``microAptiv-MCU`` CPU model.

Nothing is fitted by default. The board is described on the command line in
package pin names, because which pin carries what is a property of a product
rather than of the part.

Supported devices
-----------------

* microAptiv MCU core with its FPU, the count/compare timer and the DSP ASE
* the interrupt controller (EVIC), including its per-source vector offsets and
  both core software interrupts
* Timer1 to Timer3
* I/O ports A to G, and the peripheral pin select registers as storage
* UART1 and UART2
* SPI1 and SPI3, with the enhanced receive buffer
* the parallel master port, with address auto-increment and its one-deep read
  pipeline
* the watchdog
* clock, reset and configuration registers: SYSKEY, the oscillator and PLL
  registers, the peripheral bus dividers, the module disable bits

Everything else in the special function register window is an unimplemented
region, so an access to a peripheral this machine does not model is logged
rather than answered with a zero.

Machine options
---------------

``sdcard=<controller>:<pin>``
   An SD card in SPI mode, on that controller, selected by that port pin. The
   card itself comes from ``-drive if=sd``.

``expanders=<controller>:<pin>[:<address>[:<pin>[:<byte>]]][/...]``
   MCP23S08 port expanders: the controller, the port pin that selects them,
   their hardware address, the pin their interrupt output reaches, and what
   their input pins are wired to. Several may share one chip select and be told
   apart by address.

   The last field is for the boards that read something off an expander rather
   than driving one. A switch bank carrying a device or address the firmware
   reads at startup is the usual case, and a board that leaves it out reads
   zero, which may be a device the firmware refuses to be.

``sram=<words>[:<select>]``
   Static RAM on the parallel port: how many locations it has, and which
   address line the board uses as its chip select.

``leds=<pin>:<count>x<pixels>[:<pin>+...[:<pin>]]``
   WS2812 strings bit-banged from one pin through a demultiplexer: the data
   pin, how many strings of how many pixels, the address pins that select
   between them, and the active-low output enable. The select pins are
   separated by ``+`` because the machine's own options are comma-separated.

``led-order=grb|rgb``
   The order the strings expect their three bytes in. The part's own order is
   green first, which is the default; strings that take red first are common,
   and a firmware written for one of those decodes with red and green swapped
   unless this says so.

``relays=<address>[:<count>]``
   On/off outputs on one of the expanders: its hardware address, and how many
   of its eight pins are wired to something. They appear under the strings in
   the window as blocks -- filled while the output is on, an outline while it
   is off -- drawn larger than an LED so that a relay is not read as a pixel.

   The expander itself is described by ``expanders``; this only says what the
   board hung off the one it names.

``led-dump=<file>``
   Write every latched LED frame to a file, one line per string per frame::

      2114564666 led0 600xEB0000

   The number is the virtual time the frame latched, in nanoseconds, and the
   pixels are run-length encoded as ``<count>x<RRGGBB>``.

   Boards with relays write those to the same file, as the moment one moved::

      1911866262 switches 07

   The byte is every output at once, the lowest pin first. A line is written
   when one changes rather than once a frame, so the state at any other moment
   is the last line before it.

``logic-trace=<file>[:<start ms>[:<length ms>]]``
   Write a value change dump of the LED lines to a file, which is what a logic
   analyser on those pins would have recorded: the data line, the address the
   demultiplexer is decoding, the active-low enable, each of its outputs, and
   whether the data pin is listening to the port latch or to SDO4. The times
   are the virtual clock's, so a trace says what the firmware did rather than
   what the host was doing while it did it.

   The window matters more here than it looks. A bit-banged string moves its
   data line twice a microsecond for as long as a movie lasts, so tracing a
   whole run means gigabytes of waveform; the default is a quarter of a second
   from the start of the run. The file is closed as soon as the window ends,
   which is what lets a trace survive the machine being killed rather than shut
   down::

      -M pic32mk-devboard,...,logic-trace=leds.vcd:3000:400

   GTKWave, PulseView and the rest of sigrok read it as it stands.

``watchdog=on|off``
   Arm the watchdog at reset, as the configuration words would. Off by
   default: a firmware that stops feeding it is supposed to be reset, which
   during bring-up hides whatever stopped it.

Timing
------

``-icount shift=3`` is effectively required. It makes one emulated instruction
8 ns of virtual time, which is a 125 MHz core against the part's 120 MHz, and
everything else follows from that: the timers and the watchdog run on the
virtual clock, and firmware that bit-bangs a signal out of a GPIO with counted
delays -- which is how WS2812 strings are usually driven -- only has a defined
bit rate under it.

Running the XMASNg LED movie player
-----------------------------------

The firmware this machine was built for reads movies from an SD card and plays
them on eight strings of 600 LEDs. Its movie format is its own, so the tools
that write and check one live with the firmware rather than here, in the
`XMASNg <https://github.com/gdasher/XMASNg>`_ repository under ``tools/``.
``tools/mkmovie.py`` builds a movie and the card image that carries it::

   tools/mkmovie.py sd --strings 3 --frames 30 --pattern chase \
       --image sd.img --qemu-img build/qemu-img

   qemu-system-mipsel -M pic32mk-devboard,\
   sdcard=spi1:RD8,expanders=spi3:RA4:0/spi3:RA4:1,sram=1048576:0x800000,\
   leds=RA14:8x600:RA1+RB0+RB1:RA11,led-order=rgb \
       -drive file=sd.img,if=sd,format=raw \
       -bios XMasNG2.X.production.elf \
       -icount shift=3 -serial mon:stdio -display gtk

The strings appear as one row of pixels each, so the movie plays in the
window.

A movie's regions say which device they are for, and this firmware reads its
device ID off the expander at address 0 -- so a movie written for anything but
device zero needs that expander strapped to match, or the firmware refuses
every frame and the strings stay dark::

   expanders=spi3:RA4:0::1/spi3:RA4:1

Finding glitches in a movie
---------------------------

``tools/moviecheck.py``, in the same firmware repository, plays a movie and
compares what reached the LED strings with what the movie says should have
reached them::

   tools/moviecheck.py --qemu build/qemu-system-mipsel \
       --firmware XMasNG2.X.production.elf --movie movies/0 --loops 2

Every frame that does not match is reported with the movie frame it belongs
to, the string it appeared on, the virtual time it latched, and -- where the
shape of the damage says so -- what kind of damage it is: a frame latched part
way through, one rotated by a few pixels, one that is partly another frame, a
frame that never appeared, one that appeared twice, or one that simply differs.

The expected pixels come from the firmware's own transform, including the gamma
table, which is read out of the image being run rather than transcribed from
its source.

Because faults that repeat are the interesting ones, the report ends with what
the glitch positions have in common: the periods they fit, where they fall in
the movie's loop, and where they fall in the 218-frame ring the firmware stages
frames through in the external SRAM. ``--dump`` re-analyses a dump from an
earlier run without playing it again.

Debugging
---------

The firmware is an ELF with full debugging information, and MIPS has a
gdbstub, so ``-s -S`` and ``gdb XMasNG2.X.production.elf`` gives source-level
stepping through the application and its RTOS.

Firmware images
---------------

``-bios`` takes an ELF, an Intel HEX file or a raw binary. Only the parts of
an image that belong in flash are loaded: an XC32 ELF also describes where its
initialised data goes in RAM and where the interrupt vector offsets go in the
interrupt controller, and the startup code copies both out of flash itself
before ``main()``.
