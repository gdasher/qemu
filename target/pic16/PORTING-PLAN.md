# PIC16F17546 emulation in QEMU — porting plan

Out-of-tree fork, branch `pic16`. The driving workload is the Sandcastle polar sand
plotter firmware (`~/git/sandcastle/firmware`, XC8-built, released as Intel HEX).

## Status

| phase | state |
|---|---|
| 0 — opcode table, toolchain, fixtures | **done** (opcode table validated; gpasm fixtures outstanding) |
| 1 — target skeleton | **done** |
| 2 — instruction set | not started |
| 3 — interrupts and resets | not started |
| 4 — SoC, memory map, icount | not started |
| 5 — peripherals → **M1: boot banner** | not started |
| 6 — external world → M3: homing | not started |
| 7 — tests and docs | not started |

Agreed decisions: boot banner is the first milestone; `-icount` is a hard requirement;
the work lives on a branch in this checkout.

---

## 1. Scope — what the firmware actually needs

The PIC16F17546 has ~40 peripherals. The firmware touches eight. Everything else becomes
`create_unimplemented_device()`, which removes roughly 80% of the chip from scope.

### Required
| peripheral | registers | why |
|---|---|---|
| Ports A/B/C | `PORTx TRISx LATx ANSELx` | step/dir/enable outputs, LIMIT1 and CTRL2_INT inputs |
| IOC | `IOCAP2 IOCAF PIE0.IOCIE PIR0.IOCIF` | rising-edge interrupt on RA2 |
| PPS | `PPSLOCK RX1PPS RC1PPS SSP1CLKPPS RB6PPS SSP1DATPPS RC2PPS` | `SystemInit()` programs all of these |
| EUSART1 | `SP1BRGL/H BAUD1CON.BRG16 TX1STA.{BRGH,TXEN} RC1STA.{CREN,SPEN,OERR} RC1REG TX1REG PIE4.RC1IE PIR4.{RC1IF,TX1IF}` | 115200 8N1 console, interrupt-driven RX |
| MSSP1 (SPI host) | `SSP1STAT.{CKE,BF} SSP1CON1 SSP1BUF` | mode 0, Fosc/64, to the LTC6820 |
| TMR1 | `T1CLK T1CON.{CKPS,TMR1ON} TMR1H/L PIE1.TMR1IE PIR1.TMR1IF` | stepper ISR tick, reloaded with `-us` for 1 MHz |
| INTCON | `GIE PEIE` | single ISR at `0x0004` polling three sources |
| WWDT | `CLRWDT`, `WDTE=ON`, ~2.1 s | fed from inside every long wait |

### Not used
TMR0, TMR2/4, ADC, DAC, OPA, CMP, CMPLP, VREFLP, FVR, CLC, CWG, NCO, PWM, CCP, SRPORT,
CRC, PMD, ZCD, APM, CLKREF, EUSART2, MSSP2, NVM/EEPROM, charge pump.

### Simplification
`RSTOSC = HFINTOSC_32MHz` in the config words and the firmware never writes `OSCCONx`, so
the oscillator is a constant 32 MHz (8 MIPS). No OSC device model needed.

---

## 2. Hazards

### H1 — Stubs must be correct, not merely present
An unimplemented region reading as zero deadlocks this firmware:
- `UartWriteByte()` spins on `PIR4bits.TX1IF` → hangs before the boot banner.
- `SpiTransfer()` spins on `SSP1STATbits.BF` → hangs inside `Ctrl2Init()`, which runs
  *before* the banner.

So M1 cannot be reached with placeholder peripherals; EUSART1 TX and MSSP1 must both
genuinely work. This is why Phase 5 is ordered the way it is.

One default is benign by luck: an unconnected `SSIBus` returns 0, and
`Ctrl2ReadStatus() == 0` means "no limit tripped". Had it returned `0xFF`,
`StepperServiceSensors()` would latch `kStepperFaultLimitOuter` and every move would fault.
Do not "helpfully" change that default.

### H2 — Timer-to-CPU rate coupling requires `-icount`
`stepper.c` reloads TMR1 with `0 - us` for a 1 µs tick and runs the motion state machine
from that ISR, against a nominal 8 MIPS main loop. Under wall-clock timers those rates are
unrelated and the queue-full handshake in `StepperEnqueue()` will not behave like hardware.
Drive TMR1 from `QEMU_CLOCK_VIRTUAL` and require `-icount shift=N` with N chosen so one
emulated instruction cycle is 125 ns of virtual time. The armed watchdog has the same
coupling problem and will spuriously reset ~2.1 s in without it.

### H3 — WS2812 bit-banging is unreproducible, and that is fine
`lights.c` masks interrupts and bit-bangs 237 LEDs with `_delay()` loops calibrated to
125 ns instruction cycles. Nothing models the strip, so no observer cares. If an LED model
is ever added it must decode by cycle counts under icount, not host time. Out of scope.

---

## 3. Design

### D1 — Naming
Target arch `pic16`, binary `qemu-system-pic16`, CPU type `pic16f1-pic16-cpu`
("enhanced mid-range"). `PIC16CPUClass` carries feature bits (program size, banks
implemented, stack depth, has-EEPROM) so one core serves the family.

### D2 — Address space
Two MMU indices, as AVR does.

- `MMU_CODE_IDX` — program memory. Word address × 2 = byte address; each 14-bit
  instruction stored as a 16-bit LE word. Confirmed against the shipped HEX: program words
  occupy byte addresses `0x0000–0x7FFF` (16K words) and five config words sit at word
  `0x8007` behind an extended-linear-address record.
- `MMU_DATA_IDX` — a 64 KB space matching the FSR view exactly:
  - `0x0000–0x1FFF` bank container — core registers `0x00–0x0B` as MMIO into
    `CPUArchState`, SFRs as device MMIO, GPR as RAM, and `0x70–0x7F` as 64
    `MemoryRegion` **aliases** onto one shared 16-byte block;
  - `0x2000–0x2FEF` — 51 aliases of the 80-byte GPR blocks (the linear window);
  - `0x8000–0xFFFF` — MMIO returning the low byte of each program word.
- `TARGET_PAGE_BITS = 10`.

Doing the common-RAM and linear aliasing with `MemoryRegion`s rather than in a helper is
what keeps ordinary loads and stores on the TCG fast path.

### D3 — File-register addressing
Direct addressing supplies a 7-bit `f`. Split at translate time:
- `f < 0x0C` → core register, **bank-independent** → direct TCG global ops, no memory
  access. `INDF0/1` and `PCL` get special paths.
- `0x0C ≤ f < 0x70` → dynamic `(bsr << 7) | f`, `qemu_ld/st MO_UB`.
- `f ≥ 0x70` → common RAM at a **constant** address, no BSR dependency.

Confirmed against compiler output: `INTCONbits.PEIE = 1` emits a bare `BSF 0x0B, 6` with no
preceding `MOVLB`, because INTCON is a core register.

Indirect access (INDF via FSR, `MOVIW`/`MOVWI`) goes through a C helper resolving
core-register aliasing, the linear window, the program-flash window and the pre/post
inc/dec modes. Simpler than AVR's `fullacc` trap-and-restart, and matches the hardware's
own extra cycle.

### D4 — Decoding
`decodetree --insnwidth 16`, patterns written as `00` + the 14 opcode bits, as
`target/avr/insn.decode` does.

### D5 — gdbstub
GDB has no PIC16 architecture. Skip gdbstub for now; rely on `-d in_asm,cpu,int,exec` and
`target/pic16/disas.c`, with `scripts/pic16/picdis.py` as the golden reference. A custom
register XML is possible later but buys little without disassembly.

### D6 — Firmware loading
`load_targphys_hex_as()` in `hw/core/loader.c` reads XC8's Intel HEX directly. No ELF is
released and there is no `EM_` machine number for PIC16, so HEX is the only path. Raw
binary also accepted. Config words at word `0x8007` must be routed to their own region.

---

## 4. Phase 0 results (done)

The opcode table is in `scripts/pic16/picdis.py`, together with a reference disassembler
and a validator.

**Two errors found in DS40002637A Table 45-3.** Both were resolved empirically against
shipped XC8 images rather than guessed:

- **`MOVLB`** — the table prints a malformed 11-bit pattern (`00 000 0k kkkk`) and the
  detail page (DS p.663) omits the encoding entirely while stating a 6-bit literal. The
  standard 5-bit PIC16F1 encoding cannot be right: it reaches only bank 31, and this
  family keeps the PPS input registers in bank 60. The real encoding is
  **`00 0001 01kk kkkk` (0x0140–0x017F)**, occupying the gap between `CLRW` (0x0100) and
  `CLRF` (0x0180). Evidence: zero opcodes in the 5-bit range; 3,120 in this range, the
  most common opcode class in the image; and a bank distribution hitting exactly 14, 15,
  59, 60 and 61 — EUSART1, MSSP1 and the PPS/ANSEL/IOC banks.
- **`BTFSS`** — printed as an 18-bit pattern (`1010 11bb bfff ffff`). It is
  `01 11bb bfff ffff`, completing the BCF/BSF/BTFSC/BTFSS progression.

**Validation.** Two independently-built images decode completely, with no word exceeding
14 bits:

| image | words | decoded |
|---|---|---|
| firmware v0.6.0 | 15,873 | 15,873 |
| firmware v0.5.0 | 11,297 | 11,297 |

Coverage alone is weak evidence — the 14-bit space is nearly fully tiled, so a
wrong-but-plausible mask could absorb everything. The semantic check is stronger:
`SystemInit()` at word 0x17B4 disassembles line-for-line against the C source, matching
every literal, bank number, register offset and bit position.

**SFR addresses recovered from real bank switches** (more reliable than the memory-map
figures): PPS *outputs* are in bank 59 — `RC1PPS` 0x1D9D, `RB6PPS` 0x1D9A, `RC2PPS`
0x1D9E; PPS *inputs* in bank 60 — `PPSLOCK` 0x1E0C, `RX1PPS` 0x1E42, `SSP1CLKPPS` 0x1E47,
`SSP1DATPPS` 0x1E48. ANSEL/IOC are bank 61.

**Outstanding from Phase 0:** six instructions are never emitted by either image — `BRW`,
`CALLW`, `CLRW`, `RESET`, `SLEEP`, `TRIS`. All are fixed-value or near-fixed encodings, but
they are untested by this route and need hand-written gpasm fixtures.

---

## 5. Remaining phases

### Phase 1 — target skeleton (done)
`configs/targets/pic16-softmmu.mak`, `configs/devices/pic16-softmmu/default.mak`, and
`target/pic16/{cpu-param.h,cpu-qom.h,cpu.h,cpu.c,helper.c,helper.h,translate.c,disas.c,
machine.c,insn.decode,meson.build,Kconfig}`.

`insn.decode` carries only NOP, MOVLW and GOTO at this stage; anything else traps through
`helper_unsupported()`. `disas.c` is already complete and deliberately table-driven rather
than sharing `insn.decode`, so the two representations of the opcode map cross-check each
other as Phase 2 fills the decoder in.

Verified: `qemu-system-pic16 -M none -cpu pic16f1` builds warning-free, creates a CPU with
the correct reset state (PC 0, TO and PD set, everything else zero), executes, and prints
`nop` under `-d in_asm` at byte addresses 0, 2, 4 … as the word-address-times-two mapping
intends. TBs hold one instruction each under `-M none` because unbacked memory is treated
as I/O; that resolves itself once Phase 4 adds real program ROM.

### Phase 2 — instruction set (the bulk, and the schedule risk)
`insn.decode`, `translate.c`, `helper.c/.h`, `disas.c`. Sub-steps: basic moves and jumps;
full ALU with C/DC/Z (SUBWF/SUBWFB use inverted borrow, DS §9.3 note 1); skips via an
`env->skip` flag as AVR does; CALL/RETURN/BRA/BRW with the 16-level stack and
over/underflow; PCL/PCLATH computed jumps (must end the TB); `ADDFSR`/`MOVIW`/`MOVWI`;
`SLEEP`/`RESET`/`CLRWDT`/`TRIS`.

`CALL` takes an 11-bit literal into PC[10:0] with PCLATH[6:3] → PC[14:11]; `CALLW` takes
W → PC[7:0] with PCLATH[6:0] → PC[14:8]. `MOVIW`/`MOVWI` modes: 00 preincrement,
01 predecrement, 10 postincrement, 11 postdecrement.

### Phase 3 — interrupts and resets
Single vector at `0x0004`; INTCON/PIR0-7/PIE0-7; shadow-register save/restore; `RETFIE`;
`cpu_has_work`; sleep/wake; reset sources visible in `PCON0/PCON1`.

### Phase 4 — SoC, memory map, icount
`hw/pic16/{Kconfig,meson.build,pic16f1_soc.c,boot.c,pic16f17546.c}`. Build the D2 address
space, wire `-icount` and a fixed 32 MHz `Clock`, load HEX.
`create_unimplemented_device()` over every unmodelled SFR range so the log reveals exactly
what the firmware touches.

### Phase 5 — peripherals → M1
`hw/gpio/pic16_port.c` (ports + ANSEL + IOC); PPS validate-and-warn in the SoC;
`hw/char/pic16_eusart.c`; `hw/ssi/pic16_mssp.c`; `hw/timer/pic16_tmr1.c` (`ptimer`);
`hw/watchdog/pic16_wwdt.c`. **M1: `-serial stdio` prints the boot banner.** All six are
needed because of H1. **M2:** G-code commands get responses.

### Phase 6 — external world
GPIO in via `qemu_irq` plus a poke interface (LIMIT1 on RB5 — **without it `G28` homing
cannot complete**); an MCP23S08 SPI expander model (~150 lines, QEMU has none); step/dir
output capture so motion can be reconstructed and compared against the host simulator.
**M3: homing and a real drawing.**

### Phase 7 — tests and docs
Functional tests modelled on `tests/functional/avr/test_uno.py`, booting a released hex and
asserting on serial output. `docs/system/target-pic16.rst`.

As a fork, also record the rebase procedure. Shared files touched so far — keep this list
current, it is what a rebase has to reconcile:

| file | change |
|---|---|
| `meson.build` | one entry in the `disassemblers` dict |
| `target/meson.build` | `subdir('pic16')` |
| `target/Kconfig` | `source pic16/Kconfig` |
| `qapi/machine.json` | `pic16` added to the `SysEmuTarget` enum |
| `hw/meson.build`, `hw/Kconfig` | new subdir (Phase 4) |

Everything else is new files under `target/pic16/`, `hw/pic16/`, `scripts/pic16/`,
`configs/targets/` and `configs/devices/`.

---

## 6. Effort

| | estimate |
|---|---|
| `target/pic16/` (core, 50 instructions, disas) | ~3,500–4,500 lines |
| `hw/pic16/` (SoC, boot, machine) | ~700 |
| six peripherals | ~1,200 |
| Phase 6 external world | ~600 |
| **total** | **~6,000–7,000** |

Phase 2 is roughly half. Phases 1, 4 and 5 are largely mechanical once the pattern is set.
