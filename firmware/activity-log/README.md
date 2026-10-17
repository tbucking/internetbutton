# activity-log

Records what the accelerometer senses, keeps a **local cache** on the device, and uploads it to the
cloud **in batches**. The requirement is in [`docs/ACTIVITY.md`](../../docs/ACTIVITY.md).

This is the data-collecting counterpart to [`motion-pulse`](../motion-pulse/): motion-pulse makes the
device *react* to movement, activity-log makes it *remember* movement.

## Build and flash

```sh
cd firmware/activity-log
../../build.sh activity-log --flash
```

or by hand:

```sh
particle compile photon . --target 2.3.1 --saveTo activity-log.bin
particle binary inspect activity-log.bin     # must say version 2302
particle flash 1f0034000e47343432313031 activity-log.bin
```

**`--target 2.3.1` is not optional** — see `docs/INIT.md` §6.

## Why the cache holds buckets, not impulses

The device cannot store raw activity data, and this is not a matter of tuning:

- There is **no filesystem** on the Photon (LittleFS is Gen 3+).
- Program flash is **not writable at runtime**, so the ~1 MB of flash is unavailable.
- Emulated EEPROM is the only persistent store and measures **2048 bytes** (`EEPROM.length()`).

A measured typing minute contained **948 impulses**, so a day is ~1.4 million. Against 2048 bytes that
is off by three orders of magnitude.

So the cache stores **1-minute buckets** of 16 bytes each:

| field | type | meaning |
| --- | --- | --- |
| `startMin` | uint32 | unix minutes at the start of the bucket |
| `sum` | uint32 | sum of impulse peak deltas (mean = `sum / count`) |
| `count` | uint16 | impulses in the bucket |
| `maxDelta` | uint16 | largest peak delta |
| `maxDz` | uint8 | largest \|dz\| while active (saturates at 255) |

`maxDz` exists because measurement showed the **Z axis** is what picks up a keystroke through the desk
— peak 12 vs 4 at rest, while X and Y barely move. See
[`motion-pulse/README.md`](../motion-pulse/README.md).

The ring holds **240 buckets = 4 hours**. RAM-only by choice: a reboot loses everything unuploaded, but
EEPROM would hold only 2.1 hours and would cost flash wear.

## An impulse is not a sample

An impulse **opens** when `delta` exceeds `thresh` and **closes** when it falls back to the floor or
turns over (drops by 3 from its own peak). Counting samples instead would report a different number for
every output data rate, which makes the numbers incomparable across settings. Counting impulses gives
roughly one count per keystroke impulse.

The accelerometer runs at **400 Hz**, not the ADXL362 power-on default of ~12.5 Hz, which filters short
impulses away before they are digitised. This is the same fix that made typing detection work at all;
`InternetButton::begin()` only soft-resets the chip and sets no rate, so the setting is applied through
`extern ADXL362 accelerometer` after `begin()`.

## Feedback

Each recorded impulse gives a brief **dim amber pulse** (capped at brightness 40, ~220 ms decay). This
firmware's job is to be tested, and a logger with no indicator gives no way to tell "detected and
stored" from "detected nothing". `led 0` silences it; `led 1` restores it.

## Cloud interface

### Functions

All return an int (Device OS 2.3.1 requires it).

| Function | Default | Meaning |
| --- | --- | --- |
| `flush` | — | publish everything pending now; returns how many were waiting |
| `get <back>` | `1` | load a closed bucket into the `bucket` variable, 1 = most recent |
| `reset` | — | zero the ring, counters and `last` |
| `thresh` | `3` | delta that counts as activity (at-rest floor is 0–2) |
| `flushmin` | `5` | minutes between automatic uploads |
| `odr` | `400` | 12, 25, 50, 100, 200 or 400 Hz |
| `led` | `1` | `0` silences the indicator |

```sh
particle call 1f0034000e47343432313031 flush
particle call 1f0034000e47343432313031 get 1
particle get   1f0034000e47343432313031 bucket
```

### Variables

| Variable | Meaning |
| --- | --- |
| `held` | closed buckets currently in the ring |
| `closed` | buckets closed since boot |
| `sent` | buckets successfully uploaded |
| `pending` | closed but not yet uploaded |
| `dropped` | buckets **overwritten before they could be sent** |
| `discards` | impulses seen **before the clock was synced** |
| `count` | impulses in the currently open bucket |
| `last` | the most recently closed bucket, as `startMin,count,sum,max,dz` |
| `bucket` | whichever bucket `get` selected |
| `timeok` | 1 once the clock is valid |
| `eepromlen` | measured `EEPROM.length()` (2048) |
| `uptime` | seconds since boot |
| `reset` / `resetdata` | why the device last restarted |
| `thresh` / `odr` / `flushmin` | current settings |

### Event

```
activity    v1,<startMin>,<n>,<count>,<sum>,<max>,<dz>;<count>,<sum>,<max>,<dz>;...
```

CSV rather than JSON because the payload cap is 1024 bytes and every byte spent on punctuation is a
bucket that does not get uploaded. Up to 50 buckets per event, at most one publish per 1.1 s.

## Losses are counted, never silent

A ring buffer overwrites its oldest entry when full, and publishing can fail. Both are counted:

- **`dropped`** — buckets overwritten before they were sent. Once a bucket is overwritten it cannot be
  sent, so the publish cursor skips it and counts the loss rather than uploading a slot that now holds
  different data.
- **`discards`** — impulses seen while the clock was still unsynced, so they have no place in time.

If both stay at zero, nothing was lost. That is the point of exposing them.

## Verified

| | |
| --- | --- |
| Buckets fill and close on the minute | Yes — `closed` increments once per minute |
| Upload path | Yes — `flush` published 2 buckets in one event, `sent` 5 → 7, `pending` 2 → 1 |
| Events arrive | Yes — `activity` events received, CSV decodes correctly |
| Nothing lost | Yes — `dropped = 0`, `discards = 0` across runs |
| LED feedback | Yes — confirmed visible by eye while typing |
| `EEPROM.length()` | Measured **2048 bytes**, not the commonly quoted 3.5 KB |
| Cache survives a reboot | **No, by design** — a reboot was observed and wiped the history |
| Cause of that reboot | **Unknown** — `resetReason` read after, `SOFTWARE`; not reproduced in 3 min |

## Gotchas

- **`maxDz` saturates.** It is a `uint8`, and a bucket containing a bump hit exactly 255, so any such
  bucket reports a lower bound rather than a value.
- **`flushmin` resets to 5 on every boot**, so `sent` looks stuck for the first 5 minutes after a
  flash. That is the cache working, not a fault.
- **Reading several counters separately is not a snapshot.** `closed` and `held` can appear
  inconsistent simply because they were read either side of a bucket closing.
- **Right after a flash, variable reads 404** for 10–20 s (`"Unable to get variable on device during
  an OTA"`). Wait for a variable that only the new firmware has.
