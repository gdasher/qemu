# PIC32MK1024GPK100 emulation in QEMU — porting plan

Out-of-tree fork. The driving workload is the XMASNg LED movie player
(`~/git/XMASNg`, XC32 + Harmony 3 + FreeRTOS, released as ELF and Intel HEX).
The goal is to run that firmware unmodified and watch it play a movie read from
a virtual SD card.

**Status: design, under review. No code written.**

This plan follows the shape of `target/pic16/PORTING-PLAN.md` — scope, hazards,
design, phased milestones — because the same discipline applies. What is
different is where the work is: on PIC16 the CPU was the whole job, and here the
CPU is nearly free.

---

## 0. What we get for nothing, and what we do not

The part is a **MIPS32 microAptiv MCU core with FPU**, running **microMIPS**.
QEMU already has the target, the microMIPS decoder, the fixed-mapping MMU, the
CP0 timer, and a MIPS gdbstub. Verified against the shipped ELF:

| fact | evidence |
|---|---|
| MIPS32r2, microMIPS, little-endian, hard-float | `readelf -hA`: `micromips, mips32r2`, `Tag_GNU_MIPS_ABI_FP: Hard float (32-bit CPU, 64-bit FPU)` |
| DSP and MCU ASEs declared | ELF ASE list: `DSP ASE, DSP R2 ASE, MCU ASE, MICROMIPS ASE` |
| fixed-mapping MMU is what PIC32 needs | `fixed_mmu_map_address()` in `target/mips/tcg/system/tlb_helper.c` already does kseg0/1 → `addr & 0x1FFFFFFF` and useg → `+0x40000000` |
| `M14K` is the closest existing CPU model | `CPU_MIPS32R2 | ASE_MICROMIPS`, `MMU_TYPE_FMT`, `Config3.VInt` |
| the reset stub is ISA-agnostic | `_reset` at `0xBFC00000` is plain MIPS32 and branches to `__reset_micromips_isa`, so boot-ISA configuration cannot strand us |
| entry and load addresses | `0xBFC00000`, with LOAD segments at `0x9D0000xx` (flash), `0x8000xxxx`/`0xA00001xx` (RAM), `0xBFC00000` (boot flash) |

So there is **no new `target/` directory**. The binary is `qemu-system-mipsel`
plus a new `hw/pic32/`.

Three core-side gaps remain, all small and all in `target/mips`:

- **the EIC vector protocol** (hazard H1 — the one real core change);
- **a CPU model** with FPU and DSP on top of `M14K`;
- **the CP0 Count rate**, which must be SYSCLK/2 = 60 MHz. `MIPSCPU` already
  carries a `count_clock`, so this is wiring, not surgery.

Two upstream oddities noticed while reading, worth fixing in the fork even
though neither blocks us:

- `target/mips/cpu.c:460` reads
  `Config3 & (1 << CP0C3_ISA) & (1 << (CP0C3_ISA + 1))`, which is always zero —
  "microMIPS on reset when Config3.ISA is 3" never fires;
- QEMU's VEIC path treats `Cause.IP` as *both* the priority and the vector
  number, which is only true for controllers that have no offset registers.

---

## 1. Scope — what the firmware actually needs

`SYS_Initialize()` names its peripherals, which makes the list exact rather than
guessed:

```
CLK_Initialize, GPIO_Initialize, SPI4, SPI3, DMAC, OCMP1, UART1,
TMR2, TMR3, SPI1, PMP, DRV_SDSPI, SYS_TIME, SYS_FS, EVIC
```

### Required, with real behaviour

| block | registers the firmware touches | why |
|---|---|---|
| **EVIC** | `INTCON.MVEC`, `IFS0-5`, `IEC0-5`, `IPC0-n`, `OFF000-n` | every interrupt; `OFFn` decides where the CPU lands |
| **CPU core timer** | CP0 `Count`/`Compare` | FreeRTOS 1 kHz tick, and `__delay_us()` busy-waits on `_CP0_GET_COUNT()` |
| **GPIO A–G** | `TRISx LATx PORTx ANSELx` + SET/CLR/INV | WS2812 data on RA14, mux select RA1/RB0/RB1, `LED_OE_N` RA11, `LED_EN` RA15, expander CS RA4, SD CS RD8, RS-485 DE/RE RF12/RF13 |
| **PPS** | `SDI1R SDI3R U1RXR U1CTSR RPB5R RPD3R RPE1R RPC0R RPB9R` | `GPIO_Initialize()` programs all nine |
| **UART1** | `U1MODE U1STA U1BRG U1TXREG U1RXREG` | the debug console, 115200 8N1, polled TX (`UTXBF`, `TRMT`) |
| **SPI1** | `SPI1CON SPI1CON2 SPI1STAT SPI1BRG SPI1BUF` with **ENHBUF** | the SD card; polled on `SPI1STAT.RXBUFELM` |
| **SPI3** | same set, no enhanced buffer needed | two MCP23S08 expanders (device ID, relays) |
| **PMP** | `PMCON PMMODE PMADDR PMDIN PMDOUT PMAEN` | the external 2 MB SRAM that buffers frames |
| **TMR2** | `T2CON TMR2 PR2` + IRQ 9 | Harmony `SYS_TIME` |
| **TMR3** | `T3CON TMR3 PR3` | clock source for OCMP1 |
| **OCMP1** | `OC1CON OC1R OC1RS` | `TogglePWM()` — currently commented out in the app, but initialised |
| **WDT** | `WDTCON`, key `0x5743`, `FWDTEN=ON`, `WDTPS=PS8192` | fed once per output frame, deliberately: any state that stops emitting frames is supposed to reset the board |
| **CRU / config** | `SYSKEY OSCCON SPLLCON PMD1-7 CFGCON CHECON` | `CLK_Initialize()` does the unlock dance and disables unused modules |

### Required as storage only

`DMAC` — `DMAC_Initialize()` runs and two channel callbacks are registered, but
**every `DMAC_ChannelTransfer()` call in `app.c` is commented out**: the PMP is
driven by a plain CPU loop today. Registers must accept writes and the channels
must stay quiet. Full transfer emulation is a stretch goal, not a milestone.

`SPI4` — initialised, callback registered, and the only call site
(`SPI4_WriteSync`) is commented out.

`DATAEE`, `CHECON`/prefetch, `PMDx`, `DEVCFG` — read/write storage.

### Not used at all

ADC, all 12 PWM units, QEI, CAN FD, USB, I2C, comparators, DAC, CTMU, HLVD,
CRC hardware (the app computes CRC16 in software), RTCC, the second UART, SPI2,
SPI5/6, timers 1 and 4–9, input capture, DMT.

Everything in that list becomes `create_unimplemented_device()`, which — as on
PIC16 — is what turns "the firmware touched something we did not model" from a
silent zero into a log line.

---

## 2. Hazards

### H1 — QEMU's EIC does not have vector offsets, and PIC32 is all offsets

PIC32 runs the core in **EIC mode**: the EVIC resolves priority across ~190
sources and presents the core with a requested priority level *and* a vector
offset taken from that source's `OFFn` register. The linker script confirms the
firmware depends on it — `_vector_spacing = 0x0001`, and the image contains a
LOAD segment at `0xBF810540` (that is `OFF000`) whose 0x400 bytes are the
per-source offsets.

QEMU's `EXCP_EXT_INTERRUPT` path
(`target/mips/tcg/system/tlb_helper.c:1142`) computes
`offset = 0x200 + vector * (IntCtl.VS << 5)` and, in VEIC mode, takes the vector
straight from `Cause.IP`. Six bits of vector cannot address 190 handlers, and
the spacing is not how this part works at all. Left alone, **every interrupt
would land on the wrong handler**.

The fix is small and local: give `CPUMIPSState` an EIC input
(`eic_ripl`, `eic_offset`, `eic_active`), let the EVIC set it through a new
`cpu_mips_eic_request()`, and use `eic_offset` in place of the computed offset
when the controller supplied one. About 30 lines across `cpu.h`,
`tlb_helper.c` and a new accessor. Everything else — `pending > status`
comparison, `Status.IPL`, `EXL`, `EPC` — already behaves.

Unknown to settle in phase 1: whether Harmony/FreeRTOS needs the PIC32 **8-bit
IPL width** (`Config3.IPLW`), which widens `Status.IPL` and `Cause.RIPL` beyond
the MIPS-standard 6 bits. The firmware only uses priorities 1–7, so the narrow
form should serve; if the port's nesting code reads the wide field we will add
it.

### H2 — Polled status bits must be real, or the guest deadlocks

The PIC16 lesson repeats verbatim, and there are more spin loops here:

- `SPI1_WriteRead()` spins on `SPI1STATbits.RXBUFELM < s` — an SPI that never
  fills its receive FIFO hangs SD card init, which is the first thing the
  read-SD-card task does;
- `UART1_Write()` spins on `U1STA.UTXBF`, and `Debug()` then spins on
  `UART1_TransmitComplete()`;
- `PMP_PortIsBusy()` is polled after **every single word** moved to or from the
  SRAM — 4803 words per frame, twice;
- `SYS_FS_Mount()` failing is *not* a hang: it retries once a second and prints
  "Unable to mount", which makes it a good early milestone rather than a wall.

So the SPI enhanced buffer is not an optional refinement. `RXBUFELM`,
`TXBUFELM`, `SPIBUSY`, `SPIRBE`/`SPITBF` must all count correctly.

### H3 — The WS2812 output is bit-banged against the instruction rate

`LED_Write()` masks interrupts and toggles RA14 with hand-counted `_nop()`
runs — 68 nops for a high, 40 for a low — calibrated to 120 MHz single-cycle
execution, then calls `__delay_centi_ns()` which busy-waits on CP0 `Count`.
Eight strings of 600 pixels, 24 bits each: **230,400 line transitions per
frame**.

Two consequences:

1. `-icount shift=3` (8 ns per instruction ≈ 125 MHz) is a hard requirement,
   exactly as on PIC16. Without it the nop runs have no defined duration and no
   decoder can tell a one from a zero.
2. CP0 `Count` must advance at 60 MHz of *virtual* time, not at QEMU's default
   rate, or `__delay_us(200)` in `Debug()` and the inter-pixel gap come out
   wrong by 100/60. `MIPSCPU::count_clock` plus `count_div` already exist for
   this; the SoC sets them.

The existing `hw/pic16/ws2812.c` decoder — which learns the one/zero threshold
from the bit period rather than assuming it — should transfer unchanged.

### H4 — The PMP read pipeline is one word deep, and the firmware knows it

```c
// reads from PMDIN read the value from the previous cycle.  This call
// will return an (ignored) dummy value but also seed the PMRDIN register
volatile uint16_t ignored = PMRDIN;
```

A PMP model that returns the addressed word immediately would shift every frame
by one word, and the firmware's CRC16 check over the buffer would fail on every
frame — silently, as a "bad frame" retry, not as a crash. The one-deep read
pipeline and the address auto-increment (`PMMODE.INCM = 1`) have to be modelled
together.

The SRAM itself: `kSRAMMaxWords = 1047054` 16-bit words ≈ 2 MB, addressed with
24 address lines (`PMAEN = 0xFFFFFF`, `PMCON.EXADR = 1`), with A23
(`kPMPChipSelect = 1 << 23`) used as a chip select.

### H5 — The watchdog is armed and is load-bearing

`FWDTEN = ON`, `WDTPS = PS8192`, and `WDT_Clear()` is called from
`InternalOutputFrame()` and from the SD card task's init — with a comment
saying that is deliberate, so that any error state which stops producing frames
reboots the board. A modelled watchdog will therefore *fire* during bring-up,
whenever we stop short of emitting frames. That is correct behaviour and a
useful signal, but it must be switchable off from the command line during
bring-up or it will mask every other failure.

### H6 — Chip select is a GPIO, not the SPI controller's

Both `DRV_SDSPI` (RD8) and the expander code (RA4) drive chip select by hand
around a transfer; `SPI1CON.MSSEN` is explicitly 0. So the board must wire the
GPIO line to the SSI peripheral's `cs` input, and the SPI controller must not
assert anything itself. Getting this backwards produces an SD card that ignores
every command with no error anywhere.

### H7 — Uncached aliases

Frame buffers are declared `__attribute__((coherent))` and land at
`0xA0000110` — the KSEG1 (uncached) view of the same RAM the cached code sees
at `0x80000000`. The fixed-mapping MMU gives us this for free *provided* the
RAM is one `MemoryRegion` at physical `0x00000000` and nothing tries to be
clever about aliasing it twice.

### H8 — The MCU ASE may contain instructions QEMU does not implement

The ELF declares the MCU ASE (`ACLR`, `ASET`, `IRET`). QEMU's microMIPS
decoder names `IRET` and nothing else. If XC32 emitted `ACLR`/`ASET` for
volatile bit manipulation we will take a Reserved Instruction exception in the
middle of otherwise-working code. Phase 0 settles this with a decode sweep of
the image rather than by waiting to be surprised.

---

## 3. Design

### D1 — Naming and layout

New directory `hw/pic32/`, mirroring `hw/pic16/`:

```
hw/pic32/pic32mk_soc.[ch]      the SoC: memory map, clocks, peripheral wiring
hw/pic32/pic32mk1024gpk100.c   the part: sizes, which peripherals are fitted
hw/pic32/pic32_devboard.c      the machine, described on the command line
hw/pic32/pic32_regs.[ch]       the SET/CLR/INV register fabric (see D3)
hw/pic32/pic32_evic.[ch]       interrupt controller
hw/pic32/pic32_gpio.[ch]       ports A-G, ANSEL, PPS
hw/pic32/pic32_uart.[ch]       UART1/2
hw/pic32/pic32_spi.[ch]        SPI1-6, with the enhanced buffer
hw/pic32/pic32_pmp.[ch]        parallel master port
hw/pic32/pic32_timer.[ch]      TMR2-9
hw/pic32/pic32_ocmp.[ch]       output compare
hw/pic32/pic32_wdt.[ch]        watchdog
hw/pic32/pic32_cru.[ch]        clocks, SYSKEY, PMD, CFGCON, CHECON
hw/pic32/pic32_dmac.[ch]       DMA (registers first, transfers later)
hw/pic32/sram_pmp.c            the external SRAM on the PMP bus
```

Devices live under `hw/pic32/` rather than in `hw/char`, `hw/ssi`, `hw/timer`,
for the same reason the PIC16 work chose that: it keeps rebases cheap by not
editing shared `meson.build` and `Kconfig` files. An upstream submission would
redistribute them.

Machine `pic32mk-devboard`, SoC type `pic32mk1024gpk100`, CPU model added to
`target/mips/cpu-defs.c.inc` as `microAptiv-MCU`.

### D2 — CPU model

Copy `M14K` and add: `Config1.FP`, an FPU with 64-bit registers (`CP1 size 64`
per the ELF), `ASE_DSP | ASE_DSPR2`, `Config3.VEIC` set (not `VInt`), and a
`CP0_PRid` matching the part. The FPU is needed for the ABI even though the
application does almost no floating-point arithmetic — hard-float means library
code can touch `$f` registers at any time.

`Config3.ISA` stays at 2 ("both, MIPS32 on reset") because `_reset` is MIPS32
and switches ISA itself.

### D3 — The SET/CLR/INV fabric, once

Every PIC32 SFR appears four times: `X`, `X+4` clear, `X+8` set, `X+C` invert.
Hand-writing that in eleven device models would be eleven chances to get it
wrong. Instead `pic32_regs.c` provides a `MemoryRegionOps` wrapper: a device
declares its registers on a 16-byte stride and gets the aliases free, with
read-modify-write done centrally and per-register write masks honoured.

This is the single largest simplification available and it should be built
first, before any peripheral.

### D4 — EVIC

Sources are numbered as the data sheet numbers them. The device holds
`IFSn`/`IECn`/`IPCn`/`OFFn`, takes one `qemu_irq` per source in a named GPIO
array, resolves the highest (priority, subpriority) among enabled+pending
sources, and calls `cpu_mips_eic_request(cpu, ripl, offset)`.

Following the PIC16 finding that PIR flags needed two kinds of line, the
device takes both `ifs` (latching — software clears the flag) and `ifs-level`
(held by the peripheral, e.g. a UART's "transmit buffer empty") arrays. Which
kind each source is comes from the data sheet, per peripheral.

Single-vector mode (`INTCON.MVEC = 0`) is supported for completeness but the
firmware sets `MVEC`.

### D5 — Memory map

Physical, with kseg0/kseg1 handled by the FMT MMU:

| region | physical | size |
|---|---|---|
| RAM | `0x00000000` | 256 KB |
| Program flash | `0x1D000000` | 1 MB (two panels) |
| SFRs | `0x1F800000` | 1 MB, `create_unimplemented_device()` under everything |
| Boot flash | `0x1FC00000` | 20 KB, config words near the top |

Peripheral base addresses are **not** taken from the data sheet's tables. The
firmware ELF's symbol table carries every SFR as an `ABS OBJECT` symbol —
`PMCON` at `0xBF82E000`, `OFF000` at `0xBF810540`, `ANSELA` at `0xBF860000`,
and so on for the whole chip. A phase-0 script (`scripts/pic32/sfrmap.py`)
extracts them, and the device models are written against that, exactly as the
PIC16 work took SFR addresses from real bank switches rather than from memory
map figures.

### D6 — Firmware loading

`load_elf()` with a translation callback stripping kseg bits, which handles the
thirteen LOAD segments including the one that writes the EVIC offset registers.
Intel HEX stays supported for released images (`dev-firmware.hex`) via the
`hw/pic16/boot.c` loader, moved somewhere shared.

### D7 — gdbstub comes free, and it is worth saying so

MIPS has a gdbstub, the ELF has full DWARF, and the sources are on disk. So
`-s -S` plus `gdb build/XMasNG2.X.production.elf` gives source-level stepping
through FreeRTOS and the app. On PIC16 there was no such thing and bring-up ran
on `-d in_asm` alone; here the bring-up cost of every later phase drops
sharply. Phase 1 should confirm it works before anything else is built.

### D8 — SD card

QEMU's `sd-card` in SPI mode behind `ssi-sd`, on the SPI1 `SSIBus`, with chip
select driven from GPIO RD8 (H6). Backed by `-drive`, which gives two useful
shapes for free:

- `-drive file=movies.img,format=raw,if=none,id=sd` — a FAT image;
- `-drive file=fat:rw:/path/to/movies,if=none,id=sd` — VVFAT, exposing a host
  directory as a FAT volume, so a movie can be dropped in a directory and
  played without ever building an image.

The firmware wants `<root>/<N>/metadata.dat` and `<root>/<N>/fseq.dat`, where
`N` is a 1–3 digit number, mounted as `/dev/mmcblka1` → `/mnt/movies`.

### D9 — LED output

Eight `ws2812` instances, each 600 pixels, all watching RA14, gated by the
mux select lines RA1/RB0/RB1 and by `LED_OE_N` (RA11, active low): a strip
decodes only while it is the selected one and output is enabled. That is a
small demux device in front of the existing decoder, not a change to it.

How the frames leave QEMU is the main open question — see §5.

### D10 — The board is described, not wired

Following `hw/pic16/pic16_devboard.c` and the change that turned the board from
a product model into something described on the command line:

```
-M pic32mk-devboard \
   -device pic32-sram-pmp,size=2M \
   -drive file=fat:rw:movies,if=none,id=sd -device ssi-sd,bus=spi1,cs=RD8 \
   -device mcp23s08,bus=spi3,cs=RA4,addr=0 \
   -device mcp23s08,bus=spi3,cs=RA4,addr=1 \
   -device ws2812-mux,din=RA14,sel=RA1:RB0:RB1,oe=RA11,strips=8,pixels=600
```

Nothing about XMASNg is compiled in. Which pin means what stays in the product
repository, exactly as the sim-bridge design argued.

### D11 — Clocks and time

`FNOSC = SPLL`, `FPLLICLK = PLL_FRC` (8 MHz), `IDIV 1 × MULT 60 ÷ ODIV 4` =
**120 MHz SYSCLK**; PBCLK for the peripherals is 60 MHz (confirmed twice:
`configPERIPHERAL_CLOCK_HZ` and `U1BRG = 129` giving 115384 baud at BRGH=1).
CP0 `Count` runs at SYSCLK/2 = 60 MHz.

Timers and the watchdog run off `QEMU_CLOCK_VIRTUAL` so that everything stays
in proportion under `-icount`, as `hw/pic16/pic16_tmr1.c` does.

---

## 4. Phases and milestones

Each phase ends at something observable, and nothing is declared done on the
strength of "the registers are implemented".

**Phase 0 — reconnaissance.** Decode-sweep the ELF for instructions QEMU cannot
translate (H8); extract the SFR map from the ELF symbol table into
`scripts/pic32/sfrmap.py`; build `qemu-system-mipsel`; confirm the `M14K` model
executes microMIPS from a hand-written fixture.

**Phase 1 — CPU model, memory map, ELF loading.**
→ **M1: the boot banner.** Reaching `Debug("Unable to mount\r\n")` on the serial
port means CP0, the FMT map, the XC32 startup, FreeRTOS's scheduler start,
UART1 and the GPIO DE line all work. It needs UART1 and GPIO but no EVIC, since
`Debug()` is polled.

**Phase 2 — EVIC and the core timer.**
→ **M2: the scheduler runs.** Three application tasks plus two Harmony tasks
alternating on a 1 kHz tick, with `vTaskDelay()` returning at the right virtual
time. This is where H1 gets settled.

**Phase 3 — SPI1, SD card, file system.**
→ **M3: "Found valid movie: 0" and "Parsed meta".** The full stack —
`DRV_SDSPI` → `SYS_FS` → FatFs → an SD card in SPI mode → a host directory.

**Phase 4 — PMP, external SRAM, SPI3 and the expanders, TMR2/3, OCMP1, WDT.**
→ **M4: a frame survives the round trip.** A frame read from the card, written
word by word into the external SRAM, read back, and passing its CRC16 — with
the watchdog armed. H4 lives or dies here.

**Phase 5 — WS2812.**
→ **M5: a movie plays.** Eight strings of 600 pixels decoded per frame, at the
movie's own frame period, for a whole file, and dumped or displayed.

**Phase 6 — tests and docs.** `tests/pic32/` with a Python runner, a golden
movie and an expected frame dump; `docs/system/target-pic32.rst`.

### Testing

Bare-metal fixtures matter far less than they did on PIC16 — the instruction
set is upstream and already tested. The tests that earn their place are
peripheral-level and end-to-end:

- a small XC32-free MIPS assembly fixture per peripheral, run on a minimal
  `pic32-test` machine like `hw/pic16/pic16_test.c`, for the register
  behaviours that are easy to get wrong: SET/CLR/INV, EVIC priority
  resolution, the SPI FIFO counters, the PMP read pipeline;
- an end-to-end run of the real firmware against a generated one-frame movie,
  comparing the decoded WS2812 output to the pixels that went in. That single
  test covers most of the machine and is the one that would catch a regression
  anywhere in the chain.

---

## 5. Decisions

Settled at review:

1. **Branch base — from `pic16`.** `ws2812.c`, `mcp23s08.c` and the simulation
   bridge apply directly, so phase 0 moves them out of `hw/pic16/` into a
   shared directory and both boards use them from there.

2. **Frames leave QEMU two ways.** Dumps to disk for the regression test, and a
   built-in display device rendering the 8 × 600 grid into a QEMU console so
   `-display gtk` shows the movie playing. The simulation bridge is *not* wired
   to the lights: nothing outside QEMU needs to react to them on this product.
   The bridge remains available for pins if a later need appears.

3. **SD content — both.** A generator script produces synthetic movies for the
   tests (seeded from the embedded movie in `src/fakemovie.c`, which gives a
   known-good frame to compare against), and real `metadata.dat` + `fseq.dat`
   files drive the demo run through VVFAT.

4. **Genericity — class-driven.** Flash and RAM sizes and the fitted peripheral
   set come from a part definition, the way `PIC16CPUClass` carries family
   features, so `pic32mk1024gpk100.c` is a table and not a model.

Assumed unless corrected:

5. **Watchdog** is modelled faithfully but the machine property defaults to
   disabled until M4, because an armed watchdog resets the board during every
   phase that stops short of emitting frames and would mask the failure we are
   actually looking at.

6. **DMAC** is registers-only. Every `DMAC_ChannelTransfer()` call in `app.c`
   is commented out, so working transfers would be untested code; it is a
   stretch goal after M5.
