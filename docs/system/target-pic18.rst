.. _PIC18-System-emulator:

PIC18 System emulator
---------------------

``qemu-system-pic18`` emulates Microchip PIC18 microcontrollers built around
the PIC18F-Q43 core: the standard (non-extended, ``XINST = 0``) PIC18
instruction set with the Q43 additions -- three 14-bit FSRs, ``MOVFFL``, a
127-entry return stack, two-level interrupt context saving and the legacy
two-vector interrupt scheme (``MVECEN = 0``).

Machines
========

The board and SoC models (``pic18f27q43``, ``pic18-devboard``) are described
with the ``hw/pic18`` sources. The target itself provides one machine:

``pic18-test``
  An instruction-test harness, not a model of any part: 128 KB of program
  flash at 0, ROM regions for the user IDs (0x200000), the configuration bytes
  (0x300000) and the EEPROM (0x380000), a flat 16 KB data RAM covering the
  whole 14-bit data space, and a test device at data addresses 0x100-0x107:

  ======  =========  ======================================================
  offset  name       write action
  ======  =========  ======================================================
  0       PUTCHAR    the byte goes to stdout
  1       EXIT       QEMU exits with the byte as its status
  2       DUMP       the CPU state goes to stderr
  3       IRQ_HIGH   bit 0 drives the CPU's high-priority request line
  4       IRQ_LOW    bit 0 drives the CPU's low-priority request line
  ======  =========  ======================================================

  The fixtures in ``tests/pic18`` are assembled with XC8's ``pic-as`` and run
  with ``tests/pic18/run-tests.py``.

CPU model
=========

``pic18q43`` is the only model. The CPU implements the core registers at
0x4D6-0x4FF (INTCON0, INTCON1, STATUS, the FSRs and their indirect operands,
BSR, WREG, PCON0, PCON1, CPUDOZE, PROD, TABLAT, TBLPTR, PCL/PCLATH/PCLATU,
STKPTR and TOS) itself; every other special function register is a device
the SoC maps into data space, and general purpose RAM starts at 0x500.

Data accesses with the access bit clear resolve at translation time
(``f < 0x60`` is Access RAM at ``0x500 + f``, ``f >= 0x60`` is the SFR half
at ``0x400 + f``); banked accesses use BSR at run time and only take a slower
path when they land in the core register window (BSR = 4, ``f >= 0xD6``).

The interrupt controller lives in the SoC and presents two level lines to the
CPU: a high-priority and a low-priority request. With ``IPEN`` clear either
vectors to 0x0008; with ``IPEN`` set the low line vectors to 0x0018, gated by
``GIEL``, and a high-priority request preempts a low-priority handler. As on
the Q43 the hardware does not clear ``GIE`` on entry: ``INTCON1.STAT`` tracks
the nesting state and ``RETFIE`` steps it back, restoring the saved context
(STATUS, WREG, BSR, FSR0-2, PRODL/H, PCLATH/U) when ``RETFIE 1`` is used.
The main and low-ISR shadow sets are readable through the SoC's
``STATUS_SHAD`` .. ``PRODH_SHAD`` registers. ``CALL k, 1`` and ``RETURN 1``
use their own one-level shadow set (``WREG_CSHAD`` etc.).

Firmware loading
================

``-bios file.hex`` loads an Intel HEX image. XC8 emits the program at its
byte addresses and the configuration bytes at 0x300000, which is where the
machines map them, so the file loads in one pass; the CPU reads CONFIG1-10
from the loaded image for ``STVREN``, ``MVECEN`` and the watchdog settings.
A raw binary is loaded into the flash instead.

Timing
======

Every instruction costs one ``-icount`` tick, including the two- and
three-word instructions and the two-cycle branches, which the hardware
charges extra for. A skipped multi-word instruction does cost extra: the
skip steps over one word and the continuation words execute as NOPs, exactly
as on the hardware. At 64 MHz the Q43 executes 16 MIPS, 62.5 ns per cycle;
``-icount shift=6`` (64 ns) is the honest setting when the peripheral timers
have to line up with the instruction stream.

Debugging
=========

There is no gdbstub. ``-d in_asm`` prints each translated block through the
target's own disassembler, whose output follows XC8's ``pic-objdump`` (the
two differ only in that ``pic-objdump`` prints backward branch displacements
as unsigned); ``-d cpu`` adds the register state; ``-d unimp,guest_errors``
reports accesses to unmodelled devices and to nothing at all.

Unimplemented
=============

- Vectored interrupts (``MVECEN = 1``): logged with ``-d unimp`` when an
  interrupt is taken; the legacy vectors are used regardless.
- ``TBLWT`` and NVM programming: the write is logged with ``-d unimp``, only
  the TBLPTR update happens.
- The extended instruction set (``XINST = 1``): its opcodes are undefined.
- ``CPUDOZE`` is stored and otherwise ignored; ``PCON0.WDTWV`` and the
  MCLR cause are the SoC's to set.
- A write to PCL through an indirect operand with a post-increment or
  post-decrement (``MOVWF POSTINC0`` with FSR0 = PCL) takes the jump but
  drops the FSR step.
