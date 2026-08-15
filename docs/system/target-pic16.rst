.. _PIC16-System-emulator:

PIC16 System emulator
---------------------

``qemu-system-pic16`` emulates Microchip PIC16 microcontrollers built around
the enhanced mid-range core, the 14-bit instruction set used by the PIC16F1xxxx
families.

Machines
========

``pic16f17546``
  The bare PIC16F17546: 16K words of program flash, 26 banks of general purpose
  RAM, I/O ports A to C with interrupt-on-change, EUSART1, MSSP1 in SPI host
  mode, and Timer1. Everything else in the special function register space is
  an unimplemented device, so an access to a peripheral this model does not
  provide is logged rather than silently reading zero.

``pic16f15354``
  The bare PIC16F15354: 4K words of program flash, 512 bytes SRAM (banks 0–6),
  I/O ports A to C, MSSP1, MSSP2, Timer0, Timer1, NCO1, WWDT, and PPS.

``xmasngfm``
  The XMASNGFMv2 FM transmitter board: a PIC16F15354 microcontroller with an
  Analog Devices ADF4002 PLL frequency synthesizer connected on MSSP2 (SPI master)
  and GPIO pins RC1 (LE), RC2 (CE), RA6 (MUXOUT / lock detect), and MSSP1
  configured in SPI slave mode attached to serial 0.

``pic16-devboard``
  A configurable PIC16 development board with a simulation bridge carrying every
  package pin, and whatever chips you say are fitted. Not a model of any product,
  and nothing is fitted by default: which pin carries which chip comes off a schematic,
  and the schematic belongs to the product rather than to QEMU.

  ``soc``
    Microcontroller SoC model: ``pic16f17546`` (default) or ``pic16f15354``.

  ``expanders``
    MCP23S08 I/O expanders, as ``chip-select[:interrupt]`` separated by ``/``.
    ``expanders=RC7:RA2/RC6`` fits two, the second with its interrupt output
    unconnected.

  ``leds``
    WS2812 addressable strips, as ``data-pin[:pixels]`` separated by ``/``.
    ``leds=RB7:237`` fits one strip of 237 pixels on RB7.

  ``adf4002``
    Analog Devices ADF4002 PLL synthesizer, as ``le[:ce[:muxout]]`` pin names.
    ``adf4002=RC1:RC2:RA6`` fits an ADF4002 on MSSP2 with LE on RC1, CE on RC2,
    and MUXOUT (lock detect) driven to RA6.

  Several of a kind may be fitted, each on its own pin. Each is named on the
  bridge after the pin that identifies it -- ``expander.RC7.GP0``,
  ``led.RB7`` -- so a model can tell them apart.

  The bridge is the second serial, so a physical model of a machine can live
  outside QEMU entirely::

    qemu-system-pic16 -M pic16-devboard,expanders=RC7:RA2,leds=RB7:237 \
        -bios firmware.hex -icount shift=3 -serial stdio \
        -chardev socket,id=rig,path=/tmp/rig.sock -serial chardev:rig

  A strip reports whole frames rather than edges: QEMU decodes the bit-banged
  waveform and tells the model what colours the strip is showing, run-length
  encoded. The decode uses the ratio of each pulse to the bit period it
  measures, so it does not depend on the ``-icount`` shift.

  See ``target/pic16/SIM-BRIDGE.md`` for the protocol, and
  ``tests/pic16/bridge_model.py`` for a reference model. With no second serial
  the board is just its chips with nothing on their pins.

``pic16-test``
  A harness for exercising the instruction set. Program flash, flat data RAM,
  and a device whose registers are putchar, exit-with-code and dump-state. Not
  a model of any real part; see ``tests/pic16``.

Firmware images
===============

Firmware is supplied with ``-bios`` and may be Intel HEX or a raw binary. HEX
is what every PIC toolchain emits, and it carries the configuration words as
well as the program, so it loads in one pass::

  qemu-system-pic16 -M pic16f17546 -bios firmware.hex \
                    -serial stdio -icount shift=3

Program memory holds each 14-bit instruction in one 16-bit little-endian word,
so a word address appears at twice that byte address. Configuration words,
the Device Information Area and the Device Configuration Information sit above
word 0x8000 and are loaded into their own region.

Timing and ``-icount``
======================

``-icount`` is strongly recommended and is required for anything timing
sensitive. Timer1 runs on the virtual clock, so without ``-icount`` its rate
bears no fixed relationship to instruction execution: firmware that drives a
state machine from a timer interrupt while the main loop runs at a nominal MIPS
figure will not behave as it does on hardware. ``-icount shift=3`` is a
reasonable starting point for a 32 MHz part.

Instruction timing itself is not modelled: every instruction costs the same,
where hardware charges two cycles for branches and taken skips.

Reset
=====

Everything the guest can do to reset the device is a power cycle of the whole
board: the ``RESET`` instruction, a stack overflow or underflow with ``STVREN``
programmed, and a watchdog expiry all reset every device in the machine and
restart the core at word 0. Data memory keeps its contents, as it does on
hardware, so firmware can tell the passes apart with a flag in RAM; program
flash and the configuration words are of course unchanged.

``PCON0`` reports which of these happened. ``RI``, ``STKOVF`` and ``STKUNF``
survive the reset they caused and are cleared by writing to ``PCON0``.

Debugging
=========

GDB has no PIC16 architecture, so there is no gdbstub. Use QEMU's own tracing
instead::

  qemu-system-pic16 -M pic16f17546 -bios firmware.hex -d in_asm,cpu

``-d unimp`` reports accesses to special function registers that are not
modelled, which is the quickest way to find out what an unfamiliar image needs.

``scripts/pic16/picdis.py`` disassembles a HEX image directly and doubles as a
validator for the opcode table::

  scripts/pic16/picdis.py validate firmware.hex
  scripts/pic16/picdis.py dis firmware.hex 0x1790 16

Data sheet errata
=================

Two encodings in the PIC16F17526/46 data sheet (DS40002637A) Table 45-3 are
printed malformed, and the per-instruction pages give no encoding at all. Both
were resolved by disassembling compiler output and are documented in
``scripts/pic16/picdis.py``:

``MOVLB``
  ``00 0001 01kk kkkk``, a six-bit literal. Not the five-bit form at
  ``0x0020`` that 32-bank PIC16F1 parts use and that gputils still emits — a
  five-bit literal cannot reach bank 60, where this family keeps the peripheral
  pin select input registers.

``BTFSS``
  ``01 11bb bfff ffff``, completing the regular BCF/BSF/BTFSC/BTFSS
  progression.

Unimplemented
=============

* No gdbstub, as above.
* Instruction timing, as above.
* Of the peripherals, only the ports, EUSART1, MSSP1 and Timer1 are modelled.
  MSSP I2C modes are not.
* The WS2812 model decodes the data line and nothing else: it has no timing
  tolerances, so a controller with badly wrong ratios is decoded rather than
  rejected the way real silicon would.
* The EUSART transmits immediately rather than at the configured baud rate.
