# PIC18F27Q43 SoC, boards and the AD9834 DDS model

These are the board-level notes for the `pic18` target. When
`docs/system/target-pic18.rst` exists this section belongs there; until then
it lives here.

## Machines

| Machine | What it is |
|---|---|
| `pic18-devboard` | A PIC18F27Q43 with a simulation bridge on every package pin and whatever chips the command line fits. Nothing fitted by default. |
| `pic18f27q43` | The same board, described as "a bare part" (no chips preset). |
| `xmasngfm2` | The devboard with `adf4002=RC1:RC2:RA6,ad9834=RA3` preset: the XMASNGFMv2 rev 1.1 FM transmitter. |

All three take the same machine properties (`-M name,key=value`, not `-device`):

| Property | Meaning |
|---|---|
| `soc` | `pic18f27q43` (the only model so far). |
| `osc-ppm` | Initial HFINTOSC offset in ppm, e.g. `15000` or `-20000`. |
| `drift-profile` | `thermal`/`warmup`, `linear`, `ripple`, `jitter`, `realistic`, `stress`, or a `key=value` list over `,`/`:` with `thermal_max`, `tau`, `linear`, `ripple_amp`, `ripple_hz`, `jitter`. Identical to the pic16 board's. |
| `adf4002` | `LE[:CE[:MUXOUT]]` pin names. The PLL sits on SPI2's SSI bus; LE and CE are port outputs, MUXOUT drives a port input. |
| `ad9834` | The FSYNC pin name (`RA3` on the radio). The DDS sits on SPI2's SSI bus; its chip select is that pin's level, whatever PPS routes there. |
| `rf-dump` | File receiving the radio's outputs, one line per event (below). |
| `fm-link` | Chardev id of the co-simulation link to the machine that masters SPI1 (`hw/chips/fm_link.c` on the other end). |

Serial ports: `-serial` 0 is the byte stream from whatever masters SPI1 (the
radio's host link; the SPI module answers each byte with the head of its
transmit FIFO); `-serial` 1 is the simulation bridge (`hw/pic16/pic16_sim_bridge.c`,
shared, same `HELLO/LINES/WATCH/DRIVE/READY`, `EDGE/STATE/RESET/TICK`,
`SET/ACK` protocol). Bridge lines are `soc.RA0`..`soc.RA7`, `soc.RB0`..`soc.RB7`,
`soc.RC0`..`soc.RC7`, `soc.RE3`.

### rf-dump and bridge events

```
<ns> dds ftw=<hex7> hz=<u>                       AD9834 FREQ register update (the FSEL-selected one)
<ns> ddsctl ctrl=<hex4>                          AD9834 control register write
<ns> lo hz=<u> r=<u> n=<u> func=<hex6> locked=<u> ADF4002 latch (LE rising edge)
```

Bridge: `DDS <ns> <hex7>` (was `NCO <ns> <hex5>` on the pic16 board) and
`ADF4002 <ns> lo=<u> r=<u> n=<u> func=<hex6> locked=<0|1>`. The AD9834 at
64 MHz MCLK gives 10 MHz for ftw `0x2800000`; the firmware's FTW is the
protocol's NCO increment shifted left by 7.

### SS1 (RC6) and the frame-end interrupt

The firmware ends a host frame on the rising edge of SS1 (RC6, interrupt-on-
change). This board reproduces the pic16-devboard's behaviour exactly:

- with a plain byte-stream master on `-serial 0` there is **no** select line
  at all -- RC6 stays at its reset input level (low, "selected") and the IOC
  never fires; the firmware runs on byte flow alone, as it does on
  `pic16-devboard`;
- with `fm-link`, the master's `C <t_ns> <sel>` messages drive RC6 (low while
  selected) and, through a fan-out, SPI1's select input (SOSIF/EOSIF);
- with the bridge, a model may `DRIVE soc.RC6` and `SET` it.

The SPI module does not gate client transfers on its select input.

## SoC (`pic18f27q43-soc`)

Memory (program space at `OFFSET_CODE`): flash 128 KB at 0, user IDs 64 B at
0x200000, configuration 16 B at 0x300000, EEPROM 1 KB at 0x380000, all ROM,
erased to 0xFF and filled by the HEX loader (`boot.c`; CONFIG1..10 are copied
into `cpu->env.config[]` from the loader's blob at board init). Data space at
`OFFSET_DATA`: an unimplemented-device catcher over 0x000..0x4D5 (`-d unimp`
names anything the firmware touches that is not modelled), GPR RAM 8 KB at
0x500. 0x4D6..0x4FF is the CPU's.

| Block | Data address | Notes |
|---|---|---|
| PMD0..PMD8 | 0x060 | storage |
| WDTCON0/1, WDTPSL/H, WDTTMR | 0x078 | `pic18-wwdt`, LFINTOSC 31 kHz, WDTE/WDTCPS from CONFIG5, CLRWDT via the CPU's `clrwdt` line, sets `env->reset_wdt` and resets on expiry |
| SPI1 | 0x080 | `pic18-spi`, `-serial 0` chardev, client for the firmware |
| SPI2 | 0x08D | `pic18-spi`, host for the firmware; ADF4002 and AD9834 on its SSI bus |
| OSCCON1/2/3, OSCTUNE, OSCFRQ, OSCSTAT, OSCEN | 0x0AD | reset 0x60/0x60/0/0/0x08/0x40/0; OSCFRQ codes 0..8 = 1/2/4/8/12/16/32/48/64 MHz; OSCTUNE TUN<5:0> two's complement, 1000 ppm a step; drift profile as pic16 |
| PPSLOCK, RA0PPS..RC7PPS | 0x200 | storage; codes 0x33 (SPI1 SS) and 0x36 (SPI2 SS) route the module's SS(out) onto the pin, live |
| INT0PPS .. (input PPS) | 0x23E, 0x42 bytes | storage (SPI1SCKPPS 0x26A, SDI 0x26B, SS 0x26C, SPI2 0x26D..0x26F) |
| TMR0L/H, T0CON0/1 | 0x318 | `pic18-tmr0`, as pic16: 8-bit period match on TMR0H, T0OUTPS, T0CS 010 FOSC/4, 011 HFINTOSC, 100 LFINTOSC, 101/110 MFINTOSC, T0CKPS 1:2^n; flag PIR3.7 |
| IPR0..IPR15 | 0x362 | reset 0xFF (all high priority) |
| SHADCON, STATUS_SHAD..PRODH_SHAD | 0x376 | through `pic18_shadow_read/write()`; SHADLO 1 = main context, 0 = low-ISR context |
| ANSELx/WPUx/ODCONx/SLRCONx/INLVLx/IOCxP/IOCxN/IOCxF | 0x400 (A), 0x408 (B), 0x410 (C), 0x420 (E) | `pic18-port` region 1 |
| PIE0..PIE15 | 0x49E | |
| PIR0..PIR15 | 0x4AE | latching flags writable; level flags read-only |
| LATA/B/C, LATE | 0x4BE, 0x4C2 | `pic18-port` region 0 |
| TRISA/B/C, TRISE | 0x4C6, 0x4CA | |
| PORTA/B/C, PORTE | 0x4CE, 0x4D2 | |

Interrupts: `PIC18_IRQ(reg, bit) = reg * 8 + bit`, 128 lines, two named
GPIO-in arrays on the SoC -- `pir` (latching) and `pir-level` (held by the
peripheral). The SoC drives the CPU's `PIC18_IRQ_HIGH` line with
OR(PIR & PIE & IPR) and `PIC18_IRQ_LOW` with OR(PIR & PIE & ~IPR). Sources:
TMR0IF PIR3.7 latching; IOCIF PIR0.7 level (any IOCxF set); SPI1RXIF/TXIF/IF
PIR3.0/1/2 level (RX FIFO not empty / TX FIFO not full / INTF & INTE);
SPI2 the same on PIR5.0/1/2.

### `pic18-spi`

Registers at base+0..12: RXB, TXB, TCNTL, TCNTH, CON0 (EN 7, LSBF 2, MST 1,
BMODE 0), CON1 (SMP 7, CKE 6, CKP 5, FST 4, SSP 2, SDIP 1, SDOP 0), CON2
(BUSY 7 ro -- always 0, transfers complete at once; SSFLT 6, SSET 2, TXR 1,
RXR 0), STATUS (TXWE 7, TXBE 5, RXRE 3, CLRBF 2 w1, RXBF 0), TWIDTH, BAUD,
INTF (SRMTIF 7, TCZIF 6, SOSIF 5, EOSIF 4, RXOIF 2, TXUIF 1), INTE, CLK.
Two-byte FIFOs each way.

Host mode: a TCNTL write loads the 11-bit counter and asserts SS(out)
(named GPIO out `ss`, level per SSP); with TXR each TXB write is shifted out
through `ssi_transfer()` while the counter is nonzero (or always with
BMODE = 1), decrementing it; RXR keeps the returned byte; at zero SS(out)
deasserts and TCZIF sets; SSET holds SS(out) asserted. Client mode:
`pic18_spi_client_transfer(s, in)` answers with the head of the TX FIFO, or
the last received byte with TXUIF when empty; the byte enters the RX FIFO or
is dropped with RXOIF. The `chardev` property connects an outside master
(`can_receive` = EN && !MST && FIFO room; each received byte's reply is
written back). `external` is the flag the co-simulation link sets.
Named GPIO in `ss-in` sets SOSIF/EOSIF on the select input's edges.

## `ad9834` (`hw/chips/ad9834.c`)

An `SSIPeripheral` with `cs_polarity = SSI_CS_LOW`; FSYNC **is** the SSI chip
select (`SSI_GPIO_CS`), there is no separate `fsync` GPIO. While selected,
bytes pair into 16-bit words MSB first. D15:D14 = 00 control (B28 0x2000,
HLB 0x1000, FSEL 0x0800, PSEL 0x0400, PIN/SW 0x0200, RESET 0x0100, SLEEP1
0x0080, SLEEP12 0x0040, OPBITEN 0x0020, SIGN/PIB 0x0010, DIV2 0x0008, MODE
0x0002), 01 FREQ0, 10 FREQ1 (B28 = 1: LSB word then MSB word, the register
updating on the second; B28 = 0: HLB picks the half), 11 PHASE0/PHASE1 by
D13. `freq[2]` 28-bit, `phase[2]` 12-bit, `control`; output frequency =
freq[FSEL] * mclk / 2^28 (`mclk-hz` property, default 64000000). RESET is a
control bit, reported through the sink. QOM read-only properties `freq0`,
`freq1`, `phase0`, `phase1`, `control`, `frequency-hz`.
`ad9834_set_latch_sink(s, fn(opaque, ftw, reg, control), opaque)` fires on
every completed frequency update (reg 0/1), phase write (reg 2/3) and control
write (reg 0xFF). Kconfig `AD9834` (selects SSI), selected by
`PIC18_DEVBOARD`.

## Running the radio firmware

```
qemu-system-pic18 -M pic18-devboard,soc=pic18f27q43,adf4002=RC1:RC2:RA6,ad9834=RA3,rf-dump=rf.txt \
    -bios out/XMASNGFMv2-selftest/default.hex -display none -nodefaults \
    -icount shift=6 -d unimp,guest_errors
```

At 64 MHz the core does 16 MIPS, 62.5 ns an instruction, so `-icount shift=6`
(64 ns) is the honest setting. The production image takes the host protocol
on `-serial chardev:spi` and the bridge on a second `-serial`, exactly as
`tests/test_emulator_integration.py` drives `pic16-devboard`.

## Known gaps

- SCK/SDO/SDI pin levels are not modelled: the SSI bus carries the data and
  those pins show their latches on the bridge.
- BUSY reads 0 and transfers are instantaneous; BAUD and CLK are stored only.
- The SPI select input does not gate client transfers and SSFLT is never set.
- Only the SS(out) PPS codes route; input PPS registers are storage (the board
  wires RC6 to SPI1's select input statically).
- MVECEN = 1 (vectored interrupts, IVTBASE) is not modelled; the firmware runs
  with MVECEN = 0.
- NVM (flash/EEPROM programming), ADC, comparators, DMA, UARTs, CLC, timers
  other than TMR0 are left to the unimplemented-device catcher.
