# simon

The classic "repeat the sequence" memory game on the Particle Photon + Internet Button.

Four buttons, four colours, four tones. The game plays a growing sequence; you repeat it back. One
mistake ends the run.

## Build and flash

```sh
cd firmware/simon
particle compile photon . --target 2.3.1 --saveTo simon.bin
particle binary inspect simon.bin        # must say version 2302
particle flash 1f0034000e47343432313031 simon.bin
```

**`--target 2.3.1` is not optional.** The Photon tops out at Device OS 2.3.1, and a binary built for a
newer version makes the cloud Safe Mode healer replace the device's firmware over the air. See
`docs/INIT.md` §6 for the reproduction.

## How to play

Press any button to start. The ring plays a sequence, then it is your turn.

The four buttons form a **D-pad**, not a row — and each one lights the LED beside it:

| Button | Silkscreen says | Position | Colour | Tone | Marker LED |
| --- | --- | --- | --- | --- | --- |
| 1 | `4` | up | red | C5 | 12 (= LEDs 1 + 11) |
| 2 | `3` | right | green | E5 | 3 |
| 3 | `2` | down | blue | G5 | 6 |
| 4 | `1` | left | yellow | C6 | 9 |

The silkscreen is **reversed** (`docs/INIT.md` §8). You never need to trust it: pressing a button lights
its marker LED, so the mapping is self-evident.

To see the markers on their own at any time, for 30 s:

```sh
particle call 1f0034000e47343432313031 map 30
```

That lights red/green/blue/yellow at the four marker positions, so the correspondence can be confirmed
by eye rather than assumed.

### This mapping was wrong once, and how it was fixed

The first version used `button i -> LED i+1`, inferred from a single reading of the library's
`3_Buttons_and_LEDs` example (`buttonOn(2)` paired with `ledOn(3)`). That was wrong for three of the
four buttons — **the lights did not correspond to the buttons**, which is what prompted the fix.

The correct pairing comes from two places in the library itself:

- `4_Good_Combination` pairs each button with an LED: `1 -> 12`, `2 -> 3`, `3 -> 6`, `4 -> 9`.
- `7_Internet` labels the same buttons **up / right / down / left**.

So the buttons sit at the 12/3/6/9 o'clock positions, and a fixed offset could never have worked.
`LED 12` is the library's special index for the top: `ledOn(12)` lights LEDs 1 and 11 together, which
is why one button lights two LEDs.

The lesson is the same one this project keeps repeating: a single example is not a specification, and
the mapping was cheap to verify (`map`) but I shipped an inference instead.

## Ring behaviour

| State | Display |
| --- | --- |
| idle | the whole ring breathing dim green — "press anything" |
| showing | the marker LED for each step, in that button's colour, with its tone |
| input | dark; your press lights that button's marker LED and sounds its tone |
| over | the whole ring blinks red 4×, then the round count shows as a bar for ~2.6 s, then idle |
| win | rainbow |

Idle breathes the **whole ring** rather than one LED on purpose: button 1's marker is LEDs 1+11, so a
lone LED 1 breathing would be ambiguous with it.

## Cloud API

Variables:

| Variable | Meaning |
| --- | --- |
| `state` | `idle` \| `showing` \| `input` \| `over` \| `win` |
| `seq` | the current sequence, 1-based, e.g. `"1,3,2"` |
| `length` | steps in the current sequence |
| `round` | rounds completed this game |
| `best` | best round count this session |
| `speed` | ms per step (playback speed) |
| `idx` | how many steps of the current sequence you have entered |

Functions:

| Function | Argument | Effect |
| --- | --- | --- |
| `start` | — | start a game (from idle / over / win) |
| `reset` | — | back to idle, round count cleared |
| `press` | `1`–`4` | inject a synthetic button press; `0` clears the press flash |
| `sound` | `on` / `off` / *blank* | set or toggle sound |
| `speed` | `120`–`1500` | ms per step; blank reports the current value |
| `map` | seconds (default 8, max 30) | light the four marker LEDs so the layout can be checked |

## Verification

Because `seq` is readable and `press` injects presses, the whole game is checkable without a human —
which is the only reason the state machine can be trusted.

**Game logic** (after the mapping fix):

```text
round 1: seq=2                        0->1  [PASS]
round 2: seq=2,3                      1->2  [PASS]
round 3: seq=2,3,2                    2->3  [PASS]
round 4: seq=2,3,2,2                  3->4  [PASS]
round 5: seq=2,3,2,2,1                4->5  [PASS]
round 6: seq=2,3,2,2,1,1              5->6  [PASS]
round 7: seq=2,3,2,2,1,1,3            6->7  [PASS]
round 8: seq=2,3,2,2,1,1,3,3          7->8  [PASS]

negative control: expected 2, pressed 3 -> state='over'  [PASS]  round=8 best=8

9/9 checks passed
```

**All four buttons, and argument validation:**

```text
button 1: idle -> 'input'  [PASS]      press '0'  -> 0    [PASS]
button 2: idle -> 'input'  [PASS]      press '5'  -> -1   [PASS]
button 3: idle -> 'input'  [PASS]      press '99' -> -1   [PASS]
button 4: idle -> 'input'  [PASS]      press 'x'  -> -1   [PASS]

8/8 checks passed
```

The negative control matters as much as the positives: eight rounds passing only shows the happy path
works. Pressing a known-wrong button and confirming `state` becomes `over` is what shows the comparison
logic actually discriminates.

### A coverage gap that the first run hid

The eight-round run above exercised only buttons **1, 2 and 3** — the random sequence happened never to
pick button 4. Everything passed `9/9` while button 4 was entirely untested. That is the "counting
instead of measuring" trap: a green tick count says nothing about coverage.

Read the *inputs* a test actually used, not just the pass count. The four-button block above exists
solely to close that gap, and it is the reason `press` accepts an explicit button number.

### Not verified

The `win` state at `MAX_SEQ = 64`. Reaching it needs 64 consecutive correct rounds, so it has not been
exercised — treat it as untested rather than working.

## Two design notes worth keeping

**No `delay()` in the playback.** The obvious implementation is flash, delay, flash, delay — but that
blocks `loop()`, and a 20-step sequence would stall the device for ~10 s. Cloud variable reads time out
during a stall, so the game would become untestable from outside, and the accelerometer and button
scanning would freeze with it. Playback is therefore a `millis()`-driven state machine
(`ST_SHOW` + `showIdx`/`showPhase`/`phaseAt`), and the whole verification above depends on that choice.

**Tones use `tone()`/`noTone()`, not the library's `playNote()`.** `playNote` blocks for the note's
length, and it has two silent traps documented in `docs/INIT.md` §8 — its second argument is a
subdivision rather than milliseconds, and note names must be uppercase because it discards its own
`toUpperCase()` result. Driving `D0` directly avoids both and keeps the timing non-blocking.

## Power

Colours are dim and brightness is capped at 90. Four LEDs lit plus a buzzer is a modest load, but
`docs/INIT.md` §12 is the reason the cap is there at all.
