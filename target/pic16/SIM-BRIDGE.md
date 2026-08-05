# PIC16 simulation bridge — protocol design

Status: **design, not implemented.** Moves to `docs/system/` once it exists.

Lets a physical model of a machine live outside QEMU, in the repository that owns the
product, while QEMU keeps only the parts that are genuinely chips.

## 1. What lives where

QEMU keeps things that exist as silicon and are described by a data sheet:

- the PIC16F17546 SoC;
- the MCP23S08 expander;
- a board that wires those two together — expander CS on RC7, INT on RA2. This is
  chip-to-chip wiring off a schematic, not product behaviour;
- a generic `pic16-sim-bridge` device.

The product repository keeps everything that is not a chip:

- which pin means what (RC4 is the radius step, RB5 is the inner limit switch);
- the mechanics: steps to position, position to switch states;
- observability: traces, motion logs, stall accounting.

**The protocol never names a product function.** It speaks in package pin names, which come
from data sheets: `soc.RA4`, `expander.GP0`. Nothing in QEMU has to know that RA4 is a
direction line.

## 2. Line namespace

Every externally visible line is `<instance>.<pin>`, where the instance is the board's QOM
child name and the pin is the manufacturer's name for it:

```
soc.RA0 .. soc.RA5      soc.RB4 .. soc.RB7      soc.RC0 .. soc.RC7
expander.GP0 .. expander.GP7
```

The board registers the set; the bridge announces it at handshake. Adding a chip adds its
pins without a protocol change.

## 3. Direction

A PIC pin's direction is not fixed — TRIS decides it, and firmware changes it at runtime.
So the bridge reports direction as well as level, and the rule is:

- a pin the guest has configured as an **output** is reported to the model and ignores
  anything the model drives;
- a pin configured as an **input** takes its level from the model.

That asymmetry is worth stating explicitly because it has already bitten: on the current
in-tree board, the mechanics model and an external `qom-set` both drive RB5, and the
mechanics wins. Under this protocol there is exactly one external driver per input line,
so the ambiguity disappears.

## 4. Synchronisation

Lock-step, with QEMU as the time master. QEMU blocks the vCPU on every reportable event
and waits for the model to answer. Under `-icount` virtual time is a function of
instructions retired, and the model is a pure function of the events and timestamps it is
given, so a run is reproducible. The model must not consult wall-clock time or unseeded
randomness.

Real-time latency is acceptable, so no attempt is made to run the model concurrently.

### Events from QEMU

```
RESET <t_ns>
STATE <t_ns> soc.RA4=0 soc.RA5=0 ...        full snapshot after reset
EDGE  <t_ns> soc.RA5=1                       one or more transitions
PULSE <t_ns> soc.RA5                         a rise and fall coalesced
DIR   <t_ns> soc.RB5=in soc.RC4=out          TRIS changed
TICK  <t_ns>                                 the deadline you asked for
BYE
```

### Replies from the model

Each reply is zero or more `SET` lines followed by exactly one `ACK`:

```
SET soc.RB5=1 expander.GP0=0
ACK
```

`ACK <deadline_ns>` additionally asks to be woken at an absolute virtual time, which is how
a model with time-driven behaviour — coasting, a sensor that changes on its own — gets
control without an edge. Absolute rather than relative so it cannot drift.

## 5. Handshake and subscription

```
QEMU  > HELLO pic16-sim-bridge 1
QEMU  > LINES soc.RA0 soc.RA1 ... expander.GP7
model > HELLO sandcastle 1
model > WATCH soc.RA5=rising soc.RC4=rising soc.RA4 soc.RC3
model > DRIVE soc.RB5 expander.GP0 expander.GP1
model > READY
```

`WATCH` is the performance lever. Without it every `LAT` write would cost a round trip;
with it, only lines the model cares about do. The optional `rising`/`falling` qualifier
halves the traffic for step lines, which is the dominant cost.

`DRIVE` declares ownership. A `SET` on a line the model did not claim is a protocol error,
which catches the two-drivers mistake at the source.

## 6. Transport

A QEMU chardev, so the model can be a unix socket, a TCP port, or a subprocess:

```
qemu-system-pic16 -M sandcastle \
    -chardev socket,id=rig,path=/tmp/rig.sock \
    -device pic16-sim-bridge,chardev=rig
```

Newline-delimited text: trivially debuggable, language-agnostic, and cheap enough. If
profiling ever says otherwise, the handshake is the place to negotiate a binary framing.

## 7. Cost

A full drawing is on the order of a million steps. With `WATCH ... rising` that is a
million round trips; at roughly 20 µs each over a unix socket, tens of seconds of overhead
on top of emulation. Acceptable for a test rig. If it stops being acceptable, the lever is
`PULSE` coalescing and then batching several edges per message, not abandoning lock-step.

## 8. Sharing the model with the host simulator

The host simulator does **not** model pins. `sim/sim_hw.c` substitutes at the *driver API*
level: it reimplements `stepper.h`, `uart.h`, `ctrl2.h` and friends, supplies the
`STEPPER_*` macros that `stepper_core.h` expands, and `src/stepper.c`, `src/uart.c`,
`src/spi.c`, `src/ctrl2.c` and `src/system.c` are never compiled into it.

QEMU is the opposite: it runs the real firmware binary, drivers and all, and sees only
pins.

So the seam the two can share is **below** the driver layer and **above** the wire: the
mechanics. Proposed as `model/machine_model.[ch]` in the product repository:

```c
typedef enum { AXIS_THETA, AXIS_RADIUS } MachineAxis;

void MachineModelReset(MachineModel *m);

/* One commanded step. False if the machine could not move -- the hard stop --
   which is what stall accounting counts. */
bool MachineModelStep(MachineModel *m, MachineAxis axis, bool positive);

/* Time-driven behaviour, for the ACK <deadline> path. */
void MachineModelAdvance(MachineModel *m, uint64_t ns);

bool MachineModelInnerLimit(const MachineModel *m);
bool MachineModelOuterLimit(const MachineModel *m);
bool MachineModelThetaIndex(const MachineModel *m);

long MachineModelSteps(const MachineModel *m, MachineAxis axis);
long MachineModelStallSteps(const MachineModel *m, MachineAxis axis);
void MachineModelWriteTrace(const MachineModel *m, FILE *out);
```

Two bindings sit on it:

- **host** — `sim_hw.c` keeps its fake drivers and calls `MachineModelStep()` from
  `EmitSteps()` and `StepperRawStep()`, and `MachineModelInnerLimit()` from
  `SimLimit1Pressed()`. The trace, motion log and segment observer move onto the model or
  stay in `sim_hw` as they are.
- **emulated** — a new bridge client speaks section 4 and maps pins to the same calls. The
  pin map is one file, and it is the only place that knows RC4 is the radius step.

The asymmetry is the point rather than a compromise. The host simulator exercises the
planner and above against fake drivers; QEMU exercises the real driver layer and the real
compiler output. With the mechanics shared, a disagreement between them is attributable to
the drivers or to code generation — which is the whole reason for having both.

## 9. Migration

1. Extract `MachineModel` from `sim_hw.c` and refactor `sim_hw` onto it. No QEMU change;
   the existing host tests are the check that nothing moved.
2. Add `pic16-sim-bridge` to QEMU and register the board's line set. `hw/pic16/sandcastle.c`
   keeps the SoC and the expander and loses the mechanics.
3. Write the bridge client against `MachineModel`.
4. Point `tests/pic16/test-sandcastle.py` at the client and retire the in-tree rig.

Step 1 is worth doing on its own even if the bridge never lands: it separates the physics
from the driver stubs, which is the part of `sim_hw.c` most worth testing directly.

## 10. Open questions

- **Does the model need to see SPI traffic?** The expander is emulated in QEMU, so its
  register state is QEMU's. If a future device is better modelled outside, the bridge needs
  a transaction event as well as pin events. Deferred until something needs it.
- **Does `MachineModel` want its own time base**, or is `Advance(ns)` enough? Only matters
  once the mechanics gain something that moves on its own.
- **Reset semantics.** A guest-initiated reset currently does not reset QEMU's peripherals
  (see the porting plan), so it is worth deciding whether `RESET` on the wire means "the
  core restarted" or "the board was power-cycled". They differ, and the model probably
  wants the second.
