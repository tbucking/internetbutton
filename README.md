# internetbutton

Bring-up notes and firmware for a **Particle Photon** with a discontinued **Particle Internet Button**
shield stacked on top.

The project started as "I plugged this in, its light is flashing green, what do I do" with a second-hand
device of unknown vintage, and turned into: a full diagnosis of why it would not connect, a Device OS
upgrade, a hardware fault traced to a USB cable, an ownership transfer, and a small collection of
firmware written for the shield.

Everything learned the hard way is written up in **[`docs/INIT.md`](docs/INIT.md)** — including the
mistakes, which are the useful part.

---

## The hardware

| | |
| --- | --- |
| Board | Particle Photon (Gen 2, STM32F205), platform id `6` |
| Device ID | `1f0034000e47343432313031` |
| Serial | `BTTN-150809-ENGW-0` — a genuine Internet Button kit |
| Shield | 11 × WS2812B NeoPixel ring on `D3` |
| Buttons | 4, on `D4`–`D7`, `INPUT_PULLUP`, active low |
| Buzzer | `D0`, driven with `tone()` |
| Accelerometer | ADXL362 over **SPI** (chip select `A2`) |

Two quirks worth knowing before anything else:

- **The shield has no firmware.** Only the Photon runs code. The shield is pure wiring and parts.
- **The shield silkscreen is wrong.** The four button labels are printed `4-3-2-1` when the truth is
  `1-2-3-4`, and the buttons form a **D-pad** at the 12/3/6/9 o'clock positions of the ring — not a
  row. See `docs/INIT.md` §8 and `firmware/simon/README.md`.

## The one rule: always pass `--target 2.3.1`

Particle lists the Photon under *"devices that must stay on 2.x LTS"* — `Current LTS 2.3.1`,
`Minimum 1.2.1` — so **2.3.1 is the supported ceiling** for this hardware.

The Particle CLI's *default* compile target for a Photon is **3.3.1**, and flashing a binary built for
it makes the cloud **Safe Mode healer** silently replace the device's Device OS over the air — with no
USB activity, so it looks like spontaneous corruption.

Reproduced deliberately, same source compiled both ways:

```text
particle compile photon . --target 2.3.1   ->  "depends on system module 2 at version 2302"   OK
particle compile photon .                  ->  "depends on system module 2 at version 3302"   would upgrade the device
```

So, every build:

```sh
cd firmware/<project>
particle compile photon . --target 2.3.1 --saveTo <project>.bin
particle binary inspect <project>.bin        # MUST say version 2302
particle flash <device-id> <project>.bin
```

`--target` prints `Targeting version: 2.3.1`. **If that line is missing from the build output, the flag
did not apply.** The `particle binary inspect` check is the authoritative one.

---

## Firmware

Four standalone projects, each a complete Particle application. Only one can run on the device at a time.

| Project | What it does | README |
| --- | --- | --- |
| [`firmware/shield-check`](firmware/shield-check/) | Hardware bring-up test: walks the 11 LEDs, latches button presses, tunes the buzzer, and reads the accelerometer — reporting all of it to the cloud so the check is not purely "does it look right" | *todo* |
| [`firmware/interval-timer`](firmware/interval-timer/) | Pomodoro-style timer. The ring is a physical progress bar; work/break phases, chimes, and auto-pause when you pick it up | [README](firmware/interval-timer/README.md) |
| [`firmware/desk-presence`](firmware/desk-presence/) | Reports whether the desk has been disturbed recently and **publishes a cloud event on transition**, so a Particle webhook can act on it | *todo* |
| [`firmware/simon`](firmware/simon/) | The "repeat the sequence" memory game — 4 buttons, 4 colours, 4 tones, 11 LEDs | [README](firmware/simon/README.md) |

`shield-check` and `desk-presence` have no README yet — their purpose is above, but the detail is not
written up.

Each project is self-contained: `project.properties`, `src/`, and a vendored copy of the
[InternetButton](https://github.com/spark/internetbutton) library under `lib/`.

### Building

`particle project create` and `particle library add` **hang** on this setup (no output, exit code `124`)
and create nothing. The projects are therefore assembled by hand, which the compiler accepts without
complaint:

```text
<project>/
  project.properties     name=<project>
  src/<name>.ino
  lib/InternetButton/    unmodified upstream library
```

The library is fetched from `https://library-archives.particle.io/libraries/InternetButton/InternetButton-0.1.10.tar.gz`
rather than via the CLI, because the CLI path hangs.

### Why so much of this is testable without touching the device

Every project exposes cloud **variables** and **functions**, so behaviour can be inspected and driven
remotely. That is not decoration — it is what makes the firmware verifiable:

- `simon` exposes the current sequence (`seq`) and a `press` function that injects a synthetic button
  press, so the game can be played and checked programmatically.
- `interval-timer` exposes `state`/`remain`/`running`/`paused`.
- `desk-presence` exposes `presence`/`last`/`published`/`pending`.

Two recurring lessons came out of this (`docs/INIT.md` §8):

- **A slow poll reports the same result for a working thing and a broken one.** Reading a button mask
  once a second caught 1 press in 80 s and made three good buttons look dead. The fix was latching
  presses in firmware, not softening the conclusion.
- **A stale index is not evidence about the thing indexed.** After a flash, the cloud record still
  listed the *previous* firmware's functions. Probe the device, not the registry.

---

## Repository layout

```
docs/INIT.md                  full bring-up log: diagnosis, faults, corrections
firmware/shield-check/        hardware verification
firmware/interval-timer/      pomodoro timer
firmware/desk-presence/       motion-based presence detection
firmware/simon/               memory game
```

## Verification status

Honest accounting, because "it compiles" is not "it works":

| | Status |
| --- | --- |
| Device OS | **2.3.1**, `normal`, online, claimed to the owner's account |
| Power stability | Brownout reboot loop **fixed** (a poor USB cable) |
| All four shield peripherals | **Verified** — 11 LEDs, 4 buttons, accelerometer, buzzer |
| `interval-timer` | State machine, pause reasons, auto-pause (both directions) verified |
| `desk-presence` | Publish path verified end-to-end; state transitions and counters verified |
| `simon` | Game logic verified incl. a wrong-press negative control; all four buttons verified |
| `simon` win state at `MAX_SEQ = 64` | **Not verified** — needs 64 correct rounds |
| Physical button geometry | Verified against the hardware, after an initial **wrong** mapping |

## Notes

- **The `InternetButton` library is vendored (LGPL, v0.1.10, Richard Whitney), not submoduled.** It is
  duplicated into each project — 15 files × 4 projects. A submodule or a shared `lib/` would be tidier;
  it is duplicated because the CLI's own library command does not work here.
- **Never commit credentials.** The IoT device's Wi-Fi credentials live on the device, not in this repo,
  and no access tokens are stored here.
- Target hardware only: **Photon / P1**. None of this applies to Gen 3+ devices (Argon, Boron), which
  run modern Device OS and have different peripherals.
