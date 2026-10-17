# interval-timer

A pomodoro-style interval timer for the Particle Photon + Internet Button shield.

The 11 LEDs are a physical progress bar — all lit when a phase starts, one going dark at a time as it
runs down. Nothing needs a screen, so the button can sit on a desk and be read at a glance.

## Build and flash

```sh
cd firmware/interval-timer
particle compile photon . --target 2.3.1 --saveTo interval-timer.bin
particle binary inspect interval-timer.bin        # must say version 2302
particle flash 1f0034000e47343432313031 interval-timer.bin
```

**`--target 2.3.1` is not optional.** The Photon tops out at Device OS 2.3.1, and a binary built for a
newer version makes the cloud Safe Mode healer replace the device's firmware over the air. See
`docs/INIT.md` §6 for the reproduction, and §8 for the library bugs noted below.

## Buttons

The shield's silkscreen is **wrong** — it prints the labels `4-3-2-1` where the truth is `1-2-3-4`
(`docs/INIT.md` §8). So, by real button number:

| Button | Silkscreen says | Action |
| --- | --- | --- |
| 1 | `4` | start / pause / resume |
| 2 | `3` | skip to the next phase |
| 3 | `2` | reset to idle (clears the cycle count) |
| 4 | `1` | sound on / off |

Pressing any button briefly lights its paired LED (button *i* → LED *i+1*), so you can identify them
without trusting the print. That flash is suppressed while a phase is running, so it never hides the
progress bar mid-session.

## LED meanings

| Appearance | Meaning |
| --- | --- |
| Slow blue pulse on LED 1 | idle, waiting to start |
| Amber bar, steady | work phase running |
| Green bar, steady | short break running |
| Violet bar, steady | long break running |
| **Bar in the phase colour, pulsing** | paused |

Bar length is the fraction of the phase remaining, rounded up, so a full bar is a fresh phase and an
empty bar is the moment of transition.

### How "paused" is shown

A steady bar is too easy to miss, so a paused timer **pulses**, and it keeps the colour of the phase it
was paused in so the bar still answers *which* phase and *how much time is left*. Only the dynamics
change — steady while running, pulsing while paused — which is the entire cue.

Two details, both deliberate:

- **It never goes fully dark.** The dim half of the pulse is 22% brightness rather than off, so a paused
  timer can never be mistaken for a dead or unpowered device. A blinking-off pattern was rejected for
  exactly that reason.
- **Pausing begins with a ~1.1 s fast burst**, then settles into a slow ~1.4 Hz pulse. The burst marks
  the *moment* of pausing, which matters most for auto-pause: you look up and can tell something just
  happened, rather than finding a quietly-pulsing ring with no sense of when it started.

Distinct from idle: idle is a **single** LED pulsing, paused is a **bar** pulsing, so the two are never
confused even though both pulse.

`paused` on the cloud API reports *why*, so the two cases stay distinguishable programmatically.

## Cloud API

Variables:

| Variable | Meaning |
| --- | --- |
| `state` | `idle` \| `work` \| `break` \| `longbreak` |
| `remain` | seconds left in the current phase |
| `cycle` | completed work intervals |
| `running` | 1 while counting, 0 when paused or idle |
| `paused` | why it is paused: `""` none, `"manual"` button, `"auto"` movement |
| `move` | last accelerometer movement delta (diagnostic) |
| `seen` | latched button presses, bit 0..3 (button identification) |

Functions:

| Function | Argument | Effect |
| --- | --- | --- |
| `start` | — | start from idle, resume, or toggle |
| `pause` | — | pause if running |
| `reset` | — | stop and return to idle |
| `skip` | — | jump to the next phase |
| `sound` | `on` / `off` / *blank* | set or toggle the buzzer |
| `autopause` | `on` / `off` / *blank* | set or toggle movement auto-pause |
| `config` | `work,break,long,perLong` seconds | change durations; blank fields keep their value |
| `clearseen` | — | reset the latched button mask |

`config` applies from the **next** phase — `reset` then `start` to restart with new values. For a quick
demonstration:

```sh
D=1f0034000e47343432313031
particle call $D config 30,15,20,2    # 30 s work, 15 s break, 20 s long break, every 2
particle call $D reset
particle call $D start
```

## Defaults

Work 25 min, short break 5 min, long break 15 min after every 4 work intervals.

## Movement auto-pause

Picking the board up pauses the timer (and it stays paused — press button 1 to resume, so carrying the
button to another room does not silently run the clock down).

The threshold is calibrated against measurement, not guessed. Sampling the per-sample delta of the
8-bit accelerometer reading while the board sat untouched:

| | Measured |
| --- | --- |
| At rest, over 40 samples | max **2**, median 1 |
| Picking the board up | **53** |

So the trigger is a delta above **15** sustained for **3 consecutive samples** at 100 ms (≈ 300 ms).
Requiring consecutive samples is what makes this robust: a single isolated spike cannot trip it. An
early guess of mine put at-rest noise at "~0-4"; a real measurement said 2, and one 40 was seen once —
worth knowing that the quiet figure is 2 and that consecutive-sample logic is the guard, not the
threshold alone.

Both directions were tested: a deliberate pick-up paused the timer at `remain=57`, and an 18-second
stationary window produced **zero** false triggers (max delta 1).

## Library notes

Two silent-failure traps in `InternetButton`, both hit during this work and documented in
`docs/INIT.md` §8:

- **`playNote`'s second argument is not milliseconds.** It is `1000*240/bpm/duration`, with `bpm`
  defaulting to 250 — so `playNote("C5", 4)` is 240 ms and `playNote("C5", 400)` is a 2.4 ms click.
  The library's own examples use `8`.
- **`playNote` blocks** for the note's length, and **note names must be uppercase** (the library's
  `toUpperCase()` result is discarded). A lowercase name matches no `case` and plays nothing.

## Power

Ring brightness is capped at 70 and the colours are chosen dim. All 11 WS2812B at full white is
~660 mA, which is the load that came closest to the brownout threshold in `docs/INIT.md` §12. The
fault there turned out to be a poor USB cable rather than the LEDs, but the cap is kept deliberately.
