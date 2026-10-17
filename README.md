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
./build.sh simon              # build
./build.sh simon --flash      # build, verify, flash
./build.sh --list             # show projects
```

**Always build through `build.sh`.** It applies `--target 2.3.1`, syncs the vendored library, and then
reads the binary's declared dependency and **refuses to hand it over if it is not 2302**. That last
check is the point: it is the one mistake here that is silent and expensive otherwise.

If you build by hand instead, `--target` prints `Targeting version: 2.3.1`. **If that line is missing
from the build output, the flag did not apply.** The authoritative check is:

```sh
particle compile photon firmware/simon --target 2.3.1 --saveTo simon.bin
particle binary inspect simon.bin        # MUST say version 2302
```

---

## Firmware

Five standalone projects, each a complete Particle application. Only one can run on the device at a time.

| Project | What it does | README |
| --- | --- | --- |
| [`firmware/shield-check`](firmware/shield-check/) | Hardware bring-up test: walks the 11 LEDs, latches button presses, tunes the buzzer, and reads the accelerometer — reporting all of it to the cloud so the check is not purely "does it look right" | *todo* |
| [`firmware/interval-timer`](firmware/interval-timer/) | Pomodoro-style timer. The ring is a physical progress bar; work/break phases, chimes, and auto-pause when you pick it up | [README](firmware/interval-timer/README.md) |
| [`firmware/desk-presence`](firmware/desk-presence/) | Reports whether the desk has been disturbed recently and **publishes a cloud event on transition**, so a Particle webhook can act on it | *todo* |
| [`firmware/simon`](firmware/simon/) | The "repeat the sequence" memory game — 4 buttons, 4 colours, 4 tones, 11 LEDs | [README](firmware/simon/README.md) |
| [`firmware/motion-pulse`](firmware/motion-pulse/) | A VU meter for motion: all 11 LEDs pulse, brightness proportional to severity. Sensitive enough to detect **typing on a keyboard next to the device** | [README](firmware/motion-pulse/README.md) |

`shield-check` and `desk-presence` have no README yet — their purpose is above, but the detail is not
written up.

Each project is self-contained: `project.properties`, `src/`, and the
[InternetButton](https://github.com/spark/internetbutton) library.

### The build system

The library is kept **once**, in `vendor/InternetButton`, and copied into a project by `build.sh` at
build time. The per-project copies are gitignored, so the repository stores a single copy.

That copying step is not laziness — `particle compile` **does not follow symlinks**, which was
established by testing rather than assumed:

```text
lrwxrwxrwx lib/InternetButton -> ../../vendor/InternetButton
src/simon.ino:39:10: fatal error: InternetButton.h: No such file or directory
```

It does have a `--followSymlinks` flag, but relying on it means every build needs *two* flags that are
easy to forget — and forgetting `--target` is the expensive one. A script that always passes both is
safer than a symlink that silently depends on remembering.

A git submodule was rejected for the same reason: it still would not land where the compiler looks
(`lib/`), so it would also need a copy step.

`particle project create` and `particle library add` **hang** on this setup (no output, exit code `124`)
and create nothing, so project scaffolding is by hand, which the compiler accepts without complaint:

```text
<project>/
  project.properties     name=<project>
  src/<name>.ino
  lib/InternetButton/    copied from vendor/ by build.sh (gitignored)
```

The vendored library came from
`https://library-archives.particle.io/libraries/InternetButton/InternetButton-0.1.10.tar.gz`.

### Why so much of this is testable without touching the device

Every project exposes cloud **variables** and **functions**, so behaviour can be inspected and driven
remotely. That is not decoration — it is what makes the firmware verifiable:

- `simon` exposes the current sequence (`seq`) and a `press` function that injects a synthetic button
  press, so the game can be played and checked programmatically.
- `interval-timer` exposes `state`/`remain`/`running`/`paused`.
- `desk-presence` exposes `presence`/`last`/`published`/`pending`.
- `motion-pulse` exposes `hist`, a 13-bin histogram of impulse magnitudes, so detection *coverage* can
  be measured instead of judged by eye — a single `peak` cannot tell "caught everything" from "caught
  one hard hit".

Two recurring lessons came out of this (`docs/INIT.md` §8):

- **A slow poll reports the same result for a working thing and a broken one.** Reading a button mask
  once a second caught 1 press in 80 s and made three good buttons look dead. The fix was latching
  presses in firmware, not softening the conclusion.
- **A stale index is not evidence about the thing indexed.** After a flash, the cloud record still
  listed the *previous* firmware's functions. Probe the device, not the registry.

---

## Repository layout

```
build.sh                      build + verify + optional flash; syncs the library
vendor/InternetButton/        the library, stored once (LGPL, v0.1.10)
docs/INIT.md                  full bring-up log: diagnosis, faults, corrections
firmware/shield-check/        hardware verification
firmware/interval-timer/      pomodoro timer
firmware/desk-presence/       motion-based presence detection
firmware/simon/               memory game
firmware/motion-pulse/        motion VU meter; detects typing
```

Per-project `lib/InternetButton/` directories exist on disk after a build but are **gitignored** —
`vendor/InternetButton` is the single source of truth.

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
| `motion-pulse` | Detection measured, not assumed: typing is **14× the hands-off impulse rate** (8.8/s vs 0.6/s), Z axis confirmed as the carrier (peak 12 vs 4) |
| `motion-pulse` axis weighting (Z×2) | **Not implemented** — a measured 3× margin is available but it would change how the ring responds to sideways motion |
| Physical button geometry | Verified against the hardware, after an initial **wrong** mapping |

## Notes

- **The `InternetButton` library is vendored (LGPL, v0.1.10, Richard Whitney)**, stored once in
  `vendor/` and copied into each project by `build.sh`. See "The build system" above for why copying
  rather than a symlink or a submodule.
- **Build through `build.sh`, not by hand.** It is the only thing standing between a typo and a silent
  Device OS upgrade.
- **Never commit credentials.** The device's Wi-Fi credentials live on the device, not in this repo, and
  no access tokens are stored here.
- Target hardware only: **Photon / P1**. None of this applies to Gen 3+ devices (Argon, Boron), which run
  modern Device OS and have different peripherals.
