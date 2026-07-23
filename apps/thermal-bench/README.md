# thermal-bench

Heat, fan and clock test tool for the PS3. Shows what the console is doing thermally,
puts it under a controlled load, and saves every run so two runs can be compared —
before vs after a repaste, or stock clocks vs an overclock.

Title id `THERMAL01`, installs to `/dev_hdd0/game/THERMAL01/`.

## What the console will tell us

| Reading | Source | Notes |
|---|---|---|
| CPU / RSX temperature | lv2 syscall 383 `sys_game_get_temperature` | zone 0 = cell, 1 = rsx; top byte whole °C, next byte 1/256ths |
| Fan duty | lv2 syscall 409 `sys_sm_get_fan_policy` | 0-255 pwm level; gated behind a manufacturing-mode check on stock lv2 |
| RSX core / memory clock | hypervisor clock registers, via the CFW lv1 read | stock 500 / 650 MHz; higher on an overclocked CFW. `cellGcmGetConfiguration` is the fallback when the register cannot be read |

There is **no fan RPM** anywhere in the system — the fan has a control wire and no sense
wire, so not even the system controller knows its real speed. Fan duty is the only fan
number that exists, and in the default automatic mode the system controller steps it up
as the console heats, so it moves live.

## What this console reports (hardware run, 2026-07-21)

CPU and RSX temperatures read fine. The fan read is **not** blocked — it answered 28%,
mode automatic, so no kernel patch is needed. The southbridge and voltage zones are
refused, so those sensors do not exist for us. The undocumented thermal-zone block (syscall
384) reads all zeros — nothing useful in it, so it is no longer read. RSX clocks came back
650 / 750 MHz, i.e. this console is already overclocked above the stock 500 / 650.

## State

Full-screen view: a stress canvas behind a semi-transparent overlay carrying the live
readouts and a dual-axis graph (temperature left, fan % right, time along the bottom),
FurMark style. Reads the sensors every 500 ms, records a graph point and a CSV row every
2 seconds, and logs every change to `dbg.txt` and the debug bridge. L1/R1 change the time
window, START switches between °C and °F, and the key above the graph says which colour is
which. X steps both load dials together, Square steps the CELL dial alone, Triangle steps
the graphics dial alone — each has four steps (off / light / medium / full burn). While the
XMB is open over the app the load suspends itself and the frame cap comes back, so the
console stays responsive.
There is no quit button — leave with the PS button; the run saves itself as it goes and the
summary is written on the way out.

Each run is written to `/dev_hdd0/tmp/thermal-bench/<date>-<time>.csv` — a header with the
conditions (date, clocks, fan mode, load), one row per sample, and a summary block (peaks,
first fan step-up). Flushed every sample so a hard lock still leaves the data. If the file
cannot be opened or a write is refused part way through, the screen says so — a run that
recorded nothing must not look like one that recorded everything.

## Safety cutoff and the console model

The console model is read at startup from the console id the factory wrote into flash
(lv2 syscall 867, packet 0x19003 — its 8th byte is the product sub code, which names the
motherboard). The model, its graphics chip and the cutoff in force are shown under the title
and written into every run file.

Every console up to the CECHH generation (COK-001 / COK-002 / SEM-001 / DIA-001, which also
covers CECHM03 and CECHQ00) carries the 90 nm graphics chip, whose solder underfill weakens
from around 70 °C. Those consoles default to a **70 °C** cutoff. DIA-002 (CECHJ) onwards is the
reworked 65 nm part and gets **88 °C**. A console we cannot identify is treated as one of the
fragile ones and gets 70 °C, and the screen says the model is unknown.

`/dev_hdd0/tmp/thermal-bench/settings.txt` overrides it — `safety-cutoff=auto` (the default) or
a number between 50 and 88. The file is created with comments on first launch and can be edited
over FTP. The override is what a console with a swapped graphics chip needs: the console id is
written once at the factory and does not change when the chip does.

Load profiles are real for CPU and GPU: the CPU load is busy worker threads running below
the UI's priority, the GPU load is heavy overlapping translucent geometry with the frame
cap removed. If either temperature reaches the safety cutoff — **or the console refuses to report a
temperature at all** — the load drops itself to off, the clocks go back to what they were
at startup, and the screen and log both say why. The tool must never be the thing that
cooks the console, and it must never keep burning while flying blind.

The SPUs are the six co-processors inside the same Cell chip as the CPU, and in a real
game they produce most of the heat, so they follow the CPU dial (2 / 4 / 6 threads). Their
program is `src/spu/spu-burn.c`, built by the separate SPU compiler and wrapped into the
app's own binary by the `BuildSpuBurn` step in the project file — nothing extra to ship or
sign. If some SPUs are already taken the app quietly settles for fewer and says so.

On startup the newest previous run is read back in and drawn behind the live one as faded,
dashed ghost lines in the same colours, aligned on elapsed time — so the gap between the solid and
the ghost is the effect of whatever changed (a repaste, a clock change).

## Changing the graphics clocks

Select + Up/Down moves the RSX core clock in 50 MHz steps, Select + Left/Right moves the
memory clock in 25 MHz steps. It takes effect immediately, no reboot.

The mechanism is a hypervisor register holding a multiplier, poked through the CFW's lv1
write syscall — lifted from webMAN-MOD's `include/feat/clock.h` (credited there to
Chattrapat Sangmanee). Because this writes below the operating system, the range is clamped
(core 300-800, memory 400-900) and each press moves exactly one step. Clocks are read back
from the same register, so the readout tracks changes; `cellGcmGetConfiguration` only knows
what was set when the app started.

A clock change is global and **stays after the app exits** — the XMB and every game launched
afterwards run at the new clocks until the console reboots. That is deliberate: the tool
exists to find an overclock and settle on it. The clocks the app started with are shown
next to the current ones so the change is never invisible, and the safety cutoff puts them
back if the console gets too hot.
