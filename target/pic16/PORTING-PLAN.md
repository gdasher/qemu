# PIC16F17546 emulation in QEMU — porting plan

Out-of-tree fork, branch `pic16`. The driving workload is the Sandcastle polar sand
plotter firmware (`~/git/sandcastle/firmware`, XC8-built, released as Intel HEX).

## Status

| phase | state |
|---|---|
| 0 — opcode table, toolchain, fixtures | **done** (opcode table validated; gpasm fixtures outstanding) |
| 1 — target skeleton | **done** |
| 2 — instruction set | **done** |
| 3 — interrupts and resets | **done** |
| 4 — SoC, memory map, icount | **done** |
| 5 — peripherals → **M1: boot banner** | **done** — M1 and M2 both reached |
| 6 — external world → M3: homing | **done** — M3 reached |
| 7 — tests and docs | functional test done; docs outstanding |

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

**Revised in Phase 2.** The helper that indirect access needs anyway (for the core
registers) turned out to be the natural home for the linear window and the common-RAM fold
too, so neither needs alias regions. What the SoC actually has to build is smaller than
this design assumed:

- common RAM only has to exist at data address `0x70-0x7F`, because direct addressing
  resolves `f >= 0x70` to a constant address and indirect addressing folds the bank away
  in `fold_common()`;
- the linear window needs no regions at all — `linear_to_banked()` maps it;
- program-memory-as-data needs no region — the helper reads code space directly.

So the SoC's data space is just per-bank SFR devices, per-bank GPR RAM, and sixteen bytes
of common RAM. Direct access stays on the TCG fast path, which was the point; only
indirect access pays for a helper call, and hardware charges it an extra cycle anyway.

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

## 4b. Phase 2 results (done)

All 50 instructions are implemented in `insn.decode`, `translate.c` and `helper.c`.

### Design points that changed
**Skips are conditional branches, not a skip flag.** AVR carries `env->skip` across
instructions and re-enters translation with a TB flag. A PIC16 skip is just a conditional
jump over one instruction, so `gen_skip_if()` emits a two-way branch and ends the block.
That costs some TB chaining but removes the flag, the TB flag bit, the VMState field and
the interrupt-window interaction entirely; there is no cross-instruction state to get
wrong. `env->skip` and `TB_FLAGS_SKIP` were removed.

**STATUS write ordering gives the data sheet's behaviour for free.** DS40002637A 9.3 says
that when STATUS is the destination of an instruction that also affects Z, DC or C, the
computed bits win over the stored value. Every ALU translation stores the result and then
sets flags, so this falls out of the ordering rather than needing a special case.

### Test harness
Running fixtures needs a machine, so a slice of Phase 4 came forward:
`hw/pic16/pic16_test.c` provides `-M pic16-test` — program flash, the config-word region,
a flat 8 KB data RAM, and a test device in the SFR slots of bank 63 whose registers are
putchar, exit-with-code and dump-state. `hw/pic16/boot.c` loads Intel HEX. This is a
harness, not a model of any part; the real SoC still belongs to Phase 4.

`tests/pic16/` holds the fixtures and `run-tests.py`. A fixture exits 0 for pass or with
the number of the check that failed. Seven fixtures pass, covering the ALU and every
STATUS effect, bit operations and all four skips, calls and the hardware stack, indirect
addressing in all its forms, RESET, and SLEEP (which is expected to halt, so the runner
treats its timeout as the pass).

The harness was verified to fail as well as pass: deliberately breaking a check reports
that check's number, and the common-RAM fixtures were confirmed to catch the bug they
were written for.

### gputils limitations
`gpasm` 1.4.0 targets older PIC16F1 parts, so fixtures work around two things:

- **MOVLB** assembles to the 5-bit encoding (`0x0020-0x003F`) that 32-bank parts use, not
  this family's 6-bit form at `0x0140`. Fixtures select banks with `movwf BSR` — BSR is a
  core register, reachable from any bank — and test MOVLB itself with `dw`. `BANKSEL` must
  be avoided for the same reason.
- **ADDFSR** is rejected for every spelling of its FSR operand, so it too is emitted
  with `dw`.

`MOVIW`/`MOVWI` do assemble, and gpasm's encodings match the table exactly, including the
mode bits — an independent confirmation of that part of the decode.

### End-to-end
The real firmware (`sandcastle-v0.6.0.hex`) runs on `-M pic16-test` through `SystemInit`,
`UartInit`, `SpiInit` and into `Ctrl2Init` without one undecoded instruction, and stops
spinning at word 0x1D8F — `BTFSS SSP1STAT, BF` inside `SpiTransfer()`, with BSR 15 and
three frames on the stack. That is hazard H1 arriving exactly where it was predicted to,
and it is the correct behaviour until MSSP1 exists.

### Known gaps
- Stack overflow and underflow wrap silently; STKOVF/STKUNF and the reset they can force
  need the SoC's PCON registers (Phase 3).
- `TRIS` decodes and logs but does nothing, pending the port model.
- Instruction timing is not modelled at all yet; that lands with `-icount` in Phase 4.

---

## 4c. Phase 3 and 4 results (done)

### Interrupts
`-M pic16f17546` now has a working interrupt path: the SoC fans PIR & PIE in, the CPU
gates on INTCON and vectors to 0x0004, and RETFIE restores the shadowed context.

**PEIE gating lives in the CPU, not the SoC.** INTCON on this family carries no per-source
enables — every source has a PIE bit — so PEIE gates all of them. Putting the check in
`cpu_interrupts_enabled()` keeps it next to INTCON and means the SoC only has to report
"some enabled PIR flag is set", with no hook needed on INTCON writes.

**PIR flags latch.** A peripheral raising its line sets the flag; only software clears it.
Flags that hardware holds rather than latches — the EUSART's TX1IF and RC1IF, which the
firmware's comments note are read-only — will have to be kept asserted by their peripheral
instead. That arrives with Phase 5.

Two details corrected against the data sheet while writing the fixture:

- **STATUS is shadowed except TO and PD** (DS 12.9), so RETFIE restores only C, DC and Z.
- **STKPTR is guest-visible and 0x1F means empty** (DS 9.5.1), so the first push wraps it
  to zero rather than counting up from it. The internal representation now matches what
  bank 63 reports, instead of needing a translation later.

`tests/pic16/t_int.asm` covers a request arriving with GIE clear, GIE enabling it, the
context surviving an ISR that deliberately trashes W, BSR, PCLATH, FSR0 and STATUS, GIE
being clear inside the handler and set again after RETFIE, and a second request being
taken. The test device gained an IRQ register to stand in for a peripheral.

### SoC
`hw/pic16/pic16f1_soc.c` builds the memory map and the parts of the chip that belong to
the core rather than to a peripheral:

- program flash sized from the class, plus the configuration-word region;
- GPR RAM for banks 0-25 only — 26 and above are unimplemented on this part;
- common RAM once, at its bank 0 address, per the revised D2;
- PIR0-7 and PIE0-7 at 0x08C and 0x096, with the interrupt fan-in;
- PCON0/PCON1, with STKOVF and STKUNF read from the CPU's stack state;
- the bank 63 window at 0x1FE4-0x1FEF: the eight shadow registers, STKPTR, TOSL and TOSH,
  all readable and writable as hardware has them.

Everything else in the banked space is `create_unimplemented_device()`, so an unmodelled
access is logged rather than silently reading zero.

### What the unimplemented log gives us
Running the real firmware on `-M pic16f17546` reaches the same place it does on the test
harness — spinning in `SpiTransfer()` — and touches exactly **26 distinct SFR addresses**
on the way, which is the Phase 5 work list:

| addresses | registers |
|---|---|
| 0x0012-0x0014, 0x0018-0x001A | TRISA/B/C, LATA/B/C |
| 0x070E-0x0712 | SP1BRGL/H, RC1STA, TX1STA, BAUD1CON |
| 0x078C, 0x078F, 0x0790 | SSP1BUF, SSP1STAT, SSP1CON1 |
| 0x1D9A, 0x1D9D, 0x1D9E | RB6PPS, RC1PPS, RC2PPS |
| 0x1E0C, 0x1E42, 0x1E47, 0x1E48 | PPSLOCK, RX1PPS, SSP1CLKPPS, SSP1DATPPS |
| 0x1E8C, 0x1E91, 0x1E93, 0x1E96, 0x1EA0 | ANSELA, IOCAP, IOCAF, ANSELB, ANSELC |

Every address matches the one derived from the compiler's bank switches in Phase 0. PIE
writes no longer appear because the SoC now claims them. TMR1 and the port reads are
absent only because the firmware never gets that far.

`-icount shift=3` runs, so the requirement from H2 is satisfiable; no peripheral yet needs
a virtual-clock timer.

### Known gaps
- Stack overflow and underflow set PCON0 but do not force a reset; STVREN is not read.
- A fixture cannot yet exercise over/underflow, because doing so needs seventeen nested
  calls and a PCON0 read on a machine that has the register — straightforward once a
  fixture runs on `-M pic16f17546` rather than the harness.
- PCON0's reset-cause bits other than STKOVF/STKUNF are storage only.
- Wake-from-sleep is untested: nothing can raise an interrupt while the guest is stopped
  until a peripheral with a timer exists.

---

## 4d. Phase 5 results (done) — M1 and M2 reached

`-M pic16f17546 -bios sandcastle-v0.6.0.hex -serial file:out.txt -icount shift=3` prints

    Sandcastle polar sand plotter v0.6.0

and the console answers over a socket chardev:

| command | response |
|---|---|
| `M115` | `FIRMWARE_NAME:Sandcastle FIRMWARE_VERSION:v0.6.0` + `ok` |
| `M114` | `X:0.00 Y:0.00 R:0.00 T:0.00 LIMIT1:0 LIMIT2:0 THETA_INDEX:0 HOMED:0` + `ok` |
| `G92 X0 Y0` then `G1 X20 F600` | `ok`, and `M114` then reports `X:20.00 R:20.00` |

Motion completing is the strongest single result here: it means TMR1 fires, the stepper
ISR runs, the queue drains and the planner advances — the whole interrupt path working
against real firmware rather than a fixture.

### Devices
All four live in `hw/pic16/` rather than the `hw/gpio`, `hw/char`, `hw/ssi` and `hw/timer`
directories the plan named. That is a deliberate change for a fork: keeping them here means
no edits to four more shared `meson.build` and `Kconfig` files, so rebases stay cheap. An
upstream submission would move them.

- **`pic16_port.c`** — one device for all three ports, because the registers are not
  contiguous per port: PORTx, TRISx and LATx are three separate runs of bank 0, while the
  pad and interrupt-on-change registers are grouped ten-per-port in bank 61. Reading a
  port returns driven levels where a pin is an output and sensed levels where it is not;
  writing a port writes its latch.
- **`pic16_eusart.c`** — asynchronous mode only. Transmission is immediate rather than
  paced by the baud generator; the divisor is stored and reported but nothing waits for
  it.
- **`pic16_mssp.c`** — SPI host mode on an `SSIBus`. I2C modes log and do nothing.
- **`pic16_tmr1.c`** — `ptimer` on `QEMU_CLOCK_VIRTUAL`, so the rate stays in proportion
  to instruction execution under `-icount`. ptimer counts down, so the register reads back
  as `0x10000` minus the remaining count.

### PIR flags needed both kinds of line
Phase 3 modelled PIR bits as latching, which is wrong for three of the four flags in use.
IOCIF, TXxIF and RCxIF are read-only on hardware: firmware dismisses them by clearing
IOCxF, writing TXxREG or reading RCxREG, never by writing PIRx. Had they stayed latched,
the IOC handler would have re-entered forever, since it only clears IOCAF.

The SoC now takes two named GPIO arrays — `pir` for latching sources and `pir-level` for
held ones — with `PIRx` reads returning the union and writes only able to clear the
latched half. TMR1IF is the one latching flag here.

### Register and interrupt details came from the compiled firmware
Rather than hunting through register-definition pages, the bit positions were read out of
the image with `scripts/pic16/picdis.py`, which is both faster and authoritative about what
the firmware expects. Disassembling the ISR gave the whole interrupt map at once —
IOCIF is PIR0 bit 4, TMR1IF is PIR1 bit 6, TX1IF and RC1IF are PIR4 bits 6 and 7 — and
`UartInit` and `StepperInit` gave BRG16, BRGH, TXEN, CREN, SPEN, TMR1ON and the CKPS field.

### PPS
Peripherals are wired to the pins the firmware selects, and the PPS registers are storage
that is *checked*: programming a routing this model does not implement logs a message
naming the register and the routing that is hardwired, rather than silently doing the wrong
thing. The six values the firmware writes are all matched.

### Known gaps
- The watchdog is not modelled. `WDTE = ON` in the configuration words and the firmware
  feeds it, but an unmodelled watchdog simply never fires, which is safe. It becomes
  necessary only for testing a hang.
- The EUSART ignores the baud divisor for timing, so a test cannot check baud correctness.
- MSSP has no device on the bus yet, so transfers read back zero — the benign value, see
  hazard H1.
- Homing still needs Phase 6: `G28` reads LIMIT1 on RB5, which nothing drives.

---

## 4e. Phase 6 results (done) — M3 reached

A second machine, `-M sandcastle`, adds the parts of the plotter the firmware can
observe. Homing now completes:

```
M114  X:0.00  ... LIMIT1:0 LIMIT2:0 THETA_INDEX:0 HOMED:0
G28   ok
M114  X:15.88 R:15.88 ... LIMIT1:1 THETA_INDEX:1 HOMED:1
G1 X40 Y0 F600 -> M114  X:40.00 R:40.00 ... LIMIT1:0
```

15.88 mm is `RADIUS_HOME_OFFSET_MM`, so the firmware ended where it believes the switch
is.

### What the board adds
- **`mcp23s08.c`** — the sensor expander on the isoSPI link, as a real SPI peripheral.
  Three-byte transactions (opcode, register, data) framed by chip select, with
  interrupt-on-change driving the INT line. CS comes from RC7 and INT goes to RA2, so the
  path the firmware uses to learn a sensor moved is closed.
- **`sandcastle.c`** — the board, plus a kinematic model of the mechanics. It counts step
  pulses on the theta and radius clock lines, tracks direction, and asserts the switches
  when the count reaches where each switch sits. That is all the firmware can observe, and
  it is what makes homing completable and step output checkable.

`hw/pic16/pic16f17546.c` stays as the bare chip, with no external hardware, which is the
right thing to run a different firmware against.

### A reset bug the test caught
The mechanics model started life as a bare `TYPE_DEVICE` realized with a NULL parent bus.
An unparented device never enters the reset tree, so its reset handler never ran and it
began from zeroed state — which here means sitting on *both* limit switches. Homing still
reported `ok`, because the firmware handles starting on the switch by moving off it first.

The test passed for the wrong reason, and only the implausible `LIMIT1:1 THETA_INDEX:1` in
the pre-homing `M114` gave it away. Making the model a `SysBusDevice` puts it on the main
system bus and therefore in the reset tree. `tests/pic16/test-sandcastle.py` now asserts
the idle switch state explicitly, so the same mistake cannot pass silently again.

### Known gaps
- Pins can be driven by an in-tree device, which is what the mechanics model is, but not
  yet from outside a running QEMU. A QOM property per port would allow scripted stimulus
  and is a small addition.
- The mechanics are kinematic: no acceleration, no missed steps, no switch bounce. Good
  enough to close the loop, not a substitute for the host simulator's physical model.
- Nothing checks the *step timing*, only the resulting position. Capturing pulse
  timestamps under `-icount` would make feed rates checkable.

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
