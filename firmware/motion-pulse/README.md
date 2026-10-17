# motion-pulse

All 11 LEDs pulse on any accelerometer activity, with brightness proportional to how hard the board
was moved. A VU meter for motion: still = dark, a light tap = a dim glow, a hard shake = full
brightness.

It is sensitive enough to detect **typing on a keyboard sitting next to the device**. Getting there
took one non-obvious fix, described below.

## Build and flash

```sh
cd firmware/motion-pulse
../../build.sh motion-pulse --flash
```

or by hand:

```sh
particle compile photon . --target 2.3.1 --saveTo motion-pulse.bin
particle binary inspect motion-pulse.bin     # must say version 2302
particle flash 1f0034000e47343432313031 motion-pulse.bin
```

**`--target 2.3.1` is not optional.** The Photon tops out at Device OS 2.3.1, and a binary built for
a newer version makes the cloud Safe Mode healer replace the device's firmware. See `docs/INIT.md`
§6.

## The bug that mattered: the sensor was running at 12.5 Hz

The first version detected hard shakes and nothing else. Light taps produced a delta of 2–3 — the
same as the noise floor — and no amount of gain helped, because gain multiplies noise and signal
together.

The cause was in the library, not the settings. `InternetButton::begin()` calls the accelerometer's
`begin()`, which performs a **soft reset only**: it sets no range, no bandwidth and no output data
rate. The ADXL362 therefore ran on its power-on default of about **12.5 Hz**.

A keystroke is a sharp, high-frequency impulse. At 12.5 Hz the sensor samples it 80 ms apart and its
anti-alias filter smooths the impulse away before it is ever digitised. **No amount of gain recovers
information the sensor never captured.**

The fix is two lines, and it has to be reachable because the wrapper never exposes it:

```cpp
extern ADXL362 accelerometer;   // file-scope global in InternetButton.cpp, not a member
```

```cpp
b.begin();
accelerometer.setOutputDatarate(XL362_FILTER_FLAG_ODR400);
accelerometer.setBandwidth(XL362_FILTER_FLAG_FBW);      // full bandwidth: pass high frequencies
accelerometer.setNoiseLevel(XL362_POWER_FLAG_NOISE_NORMAL);
```

`setOutputDatarate` must come **after** `b.begin()`, because `begin()` soft-resets the chip and would
wipe the setting back to the default.

| | before (12.5 Hz) | after (400 Hz) |
| --- | --- | --- |
| light tap | 2 | detected |
| typing | not detected | 8.8 impulses/s |
| hard shake | 27 | 27 |

The rate is settable at runtime (`odr` below) so 12.5 Hz and 400 Hz can be compared on the same
typing without a reflash.

## Tuning from the cloud

No recompile needed. Each function returns the value it settled on, so a rejected argument is visible
rather than silent.

| Function | Default | Meaning |
| --- | --- | --- |
| `odr` | `400` | 12, 25, 50, 100, 200 or 400 Hz. Blank reports the current rate. |
| `thresh` | `3` | sample delta that counts as activity |
| `gain` | `16` | delta → brightness scaling |
| `max` | `150` | brightness cap (of 255) |
| `floor` | `12` | minimum brightness for any detected activity |
| `decay` | `650` | ms to fade from full to dark |
| `colour` | `255,140,0` | base colour, `r,g,b` |
| `reset` | — | zero peaks, episodes and histogram |
| `capture` | `1` | `clear` zeroes the histogram and resumes capture; `0`/`1` pause/resume |
| `test` | — | flash the ring at the brightness cap |

```sh
particle call 1f0034000e47343432313031 gain 32
particle call 1f0034000e47343432313031 decay 120     # per-keystroke flashes, not a solid glow
particle call 1f0034000e47343432313031 odr 12        # A/B the ODR fix
```

### Variables

`delta` (last sample), `level` (brightness now), `peak` (highest delta this window), `events`
(episode count), `hits` (impulses binned), `hist` (13 bins), `pdx`/`pdy`/`pdz` (per-axis peaks this
window), plus the current `gain`/`max`/`thresh`/`decay`/`odr`/`colour`.

## How severity maps to brightness

"Severity" is the frame-to-frame change in the raw reading:

```
delta = |x - lastX| + |y - lastY| + |z - lastZ|      (8-bit ADXL362 LSB, summed over 3 axes)
target = clamp(delta * gain / 16, floor, max)
```

Brightness jumps straight to the target (instant attack, so a short tap is not missed), then fades
linearly to zero over `decay` ms. Continual motion keeps re-triggering, which is why shaking holds it
bright.

`thresh` only decides *whether* to glow; the magnitude is carried by brightness. Raising it makes the
ring harder to trigger, not brighter.

Brightness is capped at 150/255 deliberately. All 11 WS2812B at full RGB is ≈660 mA — the load that
mattered in `docs/INIT.md` §12. Raise it knowingly.

## Measuring typing detection

`peak` alone answers "did it detect anything", never "did it detect everything". So each impulse is
also binned by magnitude:

- An impulse opens when `delta` exceeds `thresh`.
- It closes when the signal falls back to the floor, **or** when it has dropped by `HIT_DROP` (3)
  from its own peak — i.e. it has turned over.

The second case matters, because episode counting cannot see individual keys: an episode only ends
after `EPISODE_GAP_MS` (400 ms) of quiet, so a whole sentence collapses into *one* event. Peak-turning
needs no gap at all. Either way the **peak** is binned, so counts are per impulse, not per sample.

`hist` is 13 comma-separated counts, one digit per bin:

| bin | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| delta ≥ | 0 | 4 | 8 | 12 | 16 | 24 | 32 | 48 | 64 | 96 | 128 | 192 | 256 |

Bin 0 is the at-rest noise floor, so a baseline run reports directly how many phantom hits to expect
before any real typing is counted.

### The test, and its cues

You cannot read a script and type at the same time, so the ring gives the cues:

- **blue flash, then 25 s dark** — hands off keyboard and device
- **3 green flashes, then 30 s** — type normally the whole time
- **red flash** — stop

Clear the histogram at each cue, then read `hist`, `hits`, `pdx`/`pdy`/`pdz` afterwards.

### Measured results

| | impulses / s | delta peak | dz peak | dx | dy |
| --- | --- | --- | --- | --- | --- |
| hands off (25 s) | 0.6 | 8 | 4 | 2 | 2 |
| typing (30 s) | 8.8 | 14 | 12 | 3 | 4 |

So typing is **14× the hands-off rate**, and coverage looks complete: 264 impulses in 30 s is
consistent with press+release on roughly 130 keystrokes. Two runs of the typing test gave 246 and 264
impulses, which is stable.

### What the data says about the next improvement

`delta` is a poor discriminator on its own, because typing impulses and ambient impulses land in the
**same bins** (both mostly 4–7). Raising `thresh` cannot separate them: at `thresh=8` you would keep
18 typing impulses out of 264. Gain cannot help either — it scales signal and noise equally.

The per-axis peaks show where the margin actually is: **Z carries the keystroke** (4 → 12, a 3×
separation) while X and Y barely move (2→3, 2→4). Summing three axes triples the sensor noise while
adding almost no signal, which is why the summed `delta` only separates baseline from typing by
1.75×.

The untried fix is therefore to weight Z — `delta = |dx| + |dy| + 2*|dz|`, or Z alone. That keeps the
VU-meter response to shakes while giving keystrokes a much wider margin. It is **not** implemented,
because it changes how the ring responds to sideways motion, and the current behaviour already meets
the original spec ("pulse on *any* kind of accelerometer activity").

## Gotchas found while measuring

- **`hist` is rebuilt once a second.** A read taken immediately after `capture clear` returns the
  *pre-clear* string. Harmless for a 25–30 s window; fatal for a test that reads straight after
  clearing. Wait ~2 s.
- **`peak` used to mean "highest since boot".** It made a typing run and the preceding baseline report
  the same `peak`, which looked like detection failing. `capture clear` now resets `peak`, `pdx`,
  `pdy`, `pdz` and `events`, so what you read describes the window you just measured.
- **Identical baseline histograms across separate runs are real**, not a stale cache. Verified by
  watching `hits` climb 296 → 389 over 18 s: reads are live.
- **`events` is not a keystroke count.** See `EPISODE_GAP_MS` above.
- **Right after a flash, variable reads 404** with `"Unable to get variable on device during an OTA"`
  for 10–20 s. Wait for a variable that only the new firmware has.
