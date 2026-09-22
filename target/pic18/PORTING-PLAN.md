# PIC18F27Q43 CPU target in QEMU — porting plan and record

The `pic18` target is the PIC18F-Q43 core as the XMASNGFMv2 firmware uses it:
the standard PIC18 instruction set (`XINST = 0`) plus the Q43 additions, and
the legacy two-vector interrupt scheme (`MVECEN = 0`). It follows the `pic16`
target file for file (`target/pic16/PORTING-PLAN.md` explains the shape; this
document only records what is different and what was decided).

## Status

Done. `qemu-system-pic18 -M pic18-test` runs all fourteen instruction
fixtures in `tests/pic18`, the disassembler round-trips the whole firmware
image against XC8's `pic-objdump`, and the firmware boots on the test machine
until it polls oscillator-status registers the harness does not model.
The SoC, peripherals and board live in `hw/pic18` and are documented there.

## 1. Scope

The firmware (XC8 v3.10, `-mcpu=18f27q43`) uses 45 distinct mnemonics; the
target implements the complete standard set of DS40002147F Table 44-2:

- byte-oriented: ADDWF ADDWFC ANDWF CLRF COMF DECF INCF IORWF MOVF MOVFF
  MOVFFL MOVWF MULWF NEGF RLCF RLNCF RRCF RRNCF SETF SUBFWB SUBWF SUBWFB
  SWAPF XORWF
- skips: CPFSEQ CPFSGT CPFSLT DECFSZ DCFSNZ INCFSZ INFSNZ TSTFSZ BTFSC BTFSS
- bit: BCF BSF BTG
- control: BC BN BNC BNN BNOV BNZ BOV BZ BRA RCALL CALL CALLW GOTO RETFIE
  RETLW RETURN
- inherent: CLRWDT DAW NOP POP PUSH RESET SLEEP
- literal: ADDFSR SUBFSR ADDLW ANDLW IORLW LFSR MOVLB MOVLW MULLW SUBLW XORLW
- table: TBLRD (all modes), TBLWT (logged, pointer only)

Not implemented, by design: the extended set (ADDULNK, SUBULNK, MOVSF, MOVSS,
PUSHL, CALLW's indexed cousins) -- `fn = 3` of ADDFSR/SUBFSR is left
undecoded so it lands in the unsupported-instruction trap; vectored
interrupts (`MVECEN = 1`) -- logged with `-d unimp`, legacy vectors used;
NVM programming.

## 2. Encodings: where they came from

The datasheet table was extracted from the PDF and then every non-obvious
encoding was checked against `pic-as -mcpu=18f27q43 -Wa,-a`:

| instruction | listing | decoded as |
|---|---|---|
| `movffl 0x2000, 0x200A` | `0068 F002 F00A` | w1 = `0x0060 \| fs>>10`, w2 = `0xF000 \| (fs&0x3FF)<<2 \| fd>>12`, w3 = `0xF000 \| (fd&0xFFF)` |
| `movffl 0x3FFF, 0x0001` | `006F FFFC F001` | same, both ends of the range |
| `lfsr 0, 0x3ABC` | `EE0E F2BC` | w1 = `0xEE00 \| fn<<4 \| k>>10`, w2 = `0xF000 \| (k&0x3FF)` |
| `call target, f` / `call target` | `ED00 F000` / `EC00 F000` | s is bit 8 of the first word |
| `return f` / `return` / `retfie f` / `retfie` | `0013 0012 0011 0010` | s is bit 0 |
| `rcall` / `bra` (forward) | `D831` / `D030` | 11-bit displacement in words from PC+2 |
| `bz bnz bc bnc bov bnov bn bnn` | `E0 E1 E2 E3 E4 E5 E6 E7` | 8-bit signed displacement |
| `goto 0x1FFFE` | `EFFF F0FF` | 20-bit word address |
| `addfsr 2, 0x23` / `subfsr 1, 0x3F` | `E8A3` / `E97F` | fn in bits 7:6 |
| `tblrd * *+ *- +*` | `0008 0009 000A 000B` | mm in bits 1:0 |
| `mulwf f,a` / `mulwf f,b` | `0234` / `0334` | a is bit 8 |
| `btg 0x56,3,a` | `7656` | b in bits 11:9 |

The one thing pic-objdump and the target disagree on is backward branches:
pic-objdump prints a negative BRA/RCALL/Bcc displacement as if unsigned
(`bnz 0x370c` for a loop to 0x350c). The datasheet says two's complement and
the firmware's delay loops only work that way, so the target's reading stands;
99 of the 2906 firmware instructions differ for exactly this reason and no
other.

## 3. Design decisions

### D1 — Byte-addressed PC, no `pc_w`

The PIC18 PC is a 21-bit byte address with bit 0 clear, so `env->pc` is the
architectural value and `get_pc`/`set_pc` are identity. Word addresses only
appear in CALL/GOTO (`k << 1`).

### D2 — Skips step one word; continuation words are NOPs

A skipped two- or three-word instruction on hardware leaves its `1111 xxxx`
words to execute as NOPs. The decoder has a `1111 ----` NOP pattern and a
skip always targets `npc + 2`, so no look-ahead is needed and the icount
cost of a skipped multi-word instruction matches the hardware's.

### D3 — Access bank at translation time, core registers never in memory

`a = 0`: `f < 0x60` is `0x500 + f`, else `0x400 + f`, both constants. Within
`0x4D6-0x4FF` the translator serves the register from `env` inline (WREG, BSR,
STATUS, FSRs, PROD, TABLAT, TBLPTR, PCLAT, INTCON0) or through a small helper
(INTCON1, PCON0/1, CPUDOZE, STKPTR, TOS). `a = 1`: `(BSR << 8) | f` at run
time, and only `f >= 0xD6` gets a `BSR == 4` branch to the helper path. The
SoC therefore never sees a core register access, and can put an
unimplemented-device catcher under the rest of the SFR space as pic16 does.

### D4 — Indirect operands are addresses, resolved by helpers

INDFn/POSTINCn/POSTDECn/PREINCn/PLUSWn are core register addresses, so they
appear as `f` in any instruction and as either end of MOVFF/MOVFFL. The
translator computes the effective address and calls `helper_ld_data` /
`helper_st_data`, which resolve core registers themselves (an FSR pointing at
FSR0L writes FSR0L; one pointing at an indirect operand reads 0). A banked
access that lands on an indirect operand (`movf 0xEF, w, b` with BSR = 4) is
a direct use of it and performs the indirect access with its side effects
(`helper_ld_banked` / `helper_st_banked`).

### D5 — No side effect before the last memory access (the io-recompile rule)

When a load or store inside a multi-instruction block hits MMIO, QEMU unwinds
to the start of that instruction and re-executes it alone
(`cpu_io_recompile`). Any state the instruction had already changed is then
changed again. The first version stepped PREINC before the access and the
`t_fsr` fixture caught FSR1 ending at 1 instead of 0 -- and `movff
POSTINC0, SPI1TXB`, the shape of every XC8 peripheral loop, would have
double-stepped in real firmware. So: every FSR update is queued
(`gen_pend_fsr`) and applied after the instruction's last access, at the
latest before any control transfer (`gen_flush_fsr`); TBLRD reads before it
updates TBLPTR; the helpers do the same. A read-modify-write that names the
same operand twice owes one update.

### D6 — The Q43 interrupt state machine, not GIE clearing

DS40002147F 11.5: "The state of GIEH/L is not changed by the hardware when
servicing an interrupt. The internal state machine is used to keep track of
execution states." The core keeps `INTCON1.STAT` (`env->intstat`): 00 main,
01 low ISR, 10 high ISR from main, 11 high ISR from low. Entry saves the
main context (STATUS, WREG, BSR, FSR0-2, PRODL/H, PCLATH/U) into
`shadow_*` when leaving main, or the low context into `shadow_lo_*` when a
high request preempts a low handler; RETFIE steps the state back and
`RETFIE 1` restores the matching set. With `IPEN = 0` every request vectors
to 0x0008 and nothing nests. `CALL k, 1` / `RETURN 1` use the separate
`WREG_CSHAD/STATUS_CSHAD/BSR_CSHAD` set the datasheet gives them (0x373-0x375),
not the interrupt shadows. The SoC reads all three sets through
`pic18_shadow_read()`/`pic18_shadow_write()` and the `cshad_*` fields.

Because GIE is not cleared, a handler that re-enables nothing still cannot be
re-entered: the state machine gates it. INTCON0 writes and RETFIE end their
block through the main loop (`need_exit`, `gen_jump_exit`) so a newly
enabled or uncovered request is taken before the next instruction.

### D7 — Wake-up does not need GIE

`cpu_exec_halt` returns true whenever a request line is up, whether or not
GIE is set (11.11); if GIE is clear execution simply continues after SLEEP.

### D8 — Stack: 127 entries, STKPTR[6:0]

The interface sketch said 31; the Q43 has 127 (9.1.3, STKPTR is seven bits,
the 128th push overflows). Entry 0 is the empty slot: TOS reads zero and a
pop from STKPTR = 0 underflows. Overflow leaves the pointer at 127 and
overwrites the top; both faults set their PCON0 bit and reset the board when
`STVREN` is set, through the same `qemu_system_reset_request` path as pic16.

### D9 — STATUS has TO and PD

The interface sketch said bits 7:5 read 0; the Q43 STATUS (7.7.7) has TO at
bit 6 and PD at bit 5, read-only, set by power-on and CLRWDT, PD cleared by
SLEEP, and STATUS_SHAD saves them. They are implemented, pic16 style.

### D10 — Reset values from the datasheet

INTCON0 resets to 0x07 (the INTxEDG bits are 1), not 0. PCON0 powers on as
0x3C (nPOR and nBOR clear); the cause bits survive resets, and the SoC may
set `env->reset_wdt` before a watchdog reset to have nRWDT read 0.

### D11 — PCL reads return the next address

`ADDWF PCL` at X reads X + 2 (9.1.2's computed GOTO), latching PCLATH/PCLATU
from it; a PCL write jumps to PCLATU:PCLATH:value. Through an FSR the same
happens in the helper, which commits the PC before the call and, for the
write, restores the instruction's state (settling icount) before leaving
the block at the new address.

### D12 — Timing

One tick per instruction, including the multi-word ones that the hardware
charges two or three cycles for; skipped multi-word instructions cost what
hardware charges (D2). `-icount shift=6` is the honest speed at 64 MHz.

## 4. Test harness

`hw/pic18/pic18_test.c` (`-M pic18-test`, `CONFIG_PIC18_TEST`): flash, user
ID, config and EEPROM ROMs in code space, a flat 16 KB RAM over the whole
data space, and a test device at 0x100-0x107 (putchar, exit, dump, IRQ high,
IRQ low). `tests/pic18/run-tests.py` assembles each `t_*.s` with `pic-as`
(found under `C:/Program Files/Microchip/xc8/v*` or `/opt/microchip/xc8/v*`,
the PIC18F-Q DFP under `~/.mchp_packs`; `PIC_AS` and `PIC18_DFP` override)
and runs it; exit code 0 is a pass, anything else is the failing check.

| fixture | covers |
|---|---|
| t_alu | every flag of every ALU instruction, DAW, TO/PD read-only |
| t_skip | all skips over 1-, 2- and 3-word instructions, STATUS untouched |
| t_ctrl | CALL/RCALL/RETURN/RETLW/CALLW, fast shadows, all branches, PUSH/POP, STKPTR/TOS, computed GOTO, PCL latch |
| t_fsr | LFSR, all five indirect modes on all FSRs, negative PLUSW, wrap, FSR into core registers, PCL via FSR |
| t_movff | MOVFF/MOVFFL between banks, access bank, WREG, indirect ends, 0x3FFF |
| t_mul | MULLW/MULWF |
| t_tblrd | all TBLRD modes, TBLPTRH carry, config bytes at 0x300000, reset vector bytes |
| t_int | both vectors, IPEN 0/1, GIEL gating, STAT, preemption, RETFIE 1 context restore |
| t_sleep, t_halt | wake without GIE, PD/TO, CLRWDT, SLEEP with nothing pending idles |
| t_stack | 127 entries, STKOVF/STKUNF with STVREN off, PCON0/PCON1 |
| t_reset, t_stvren | RESET instruction (nRI), stack overflow reset (STKOVF) |
| t_access | access-bank mapping, banked into the core window, BSR mask |

Things the fixtures taught about pic-as: `C`, `Z`, `N`, `w`, `f`, `a`, `b`,
`c` are reserved operand words (the include uses `C_BIT` etc.); MOVFF/LFSR
operands are full 12/14-bit addresses, not access-bank offsets; an
`include` resolves against the working directory, not `-I`.

## 5. Firmware

`out/XMASNGFMv2/default.hex` on `-M pic18-test` with
`-d in_asm,unimp,guest_errors`: no unsupported instruction, no guest error;
after 213 distinct instructions it sits in the loop at 0x3FFC polling two
oscillator-status SFRs (0x093 bit 7, 0x094 bit 5) that read zero from flat
RAM -- the SoC's job. `xp /5200i 0x2600` in the monitor disassembles the
whole image; every instruction agrees with `pic-objdump -d default.elf`
except the 99 backward branches of section 2.

## 6. Known gaps

- MVECEN = 1 and TBLWT/NVM are logged, not modelled.
- A PCL write through POSTINC/POSTDEC loses the FSR step (the helper leaves
  the block from inside the store).
- `DAW` leaves C alone when no high-nibble adjustment carries; the datasheet
  only says "C affected".
- Migration state is defined (`machine.c`) but has not been exercised.
