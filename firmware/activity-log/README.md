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

## How often it uploads

| | |
| --- | --- |
| Buckets close | every **1 minute** |
| Publish attempt | every **5 minutes** (`flushmin`, settable 1–60) |
| Payload per send | up to **50** buckets in one event |
| Measured cadence | **300 s**, 5 buckets per event, `pending` draining to 0 |

Measured, not just configured: three consecutive batches arrived at `02:40:39`, `02:45:39` and
`02:50:39` UTC — exactly 300 s apart, each carrying 5 contiguous one-minute buckets. The device's own
counters agreed (`sent` +5, `pending` → 0 at the same moments).

Three things modulate that cadence:

- **`flushmin` is not persisted.** It reverts to 5 on every reboot, so `sent` looks stuck for the first
  5 minutes after a flash. That is the cache working, not a fault.
- **A backlog drains in bursts.** If more than 50 buckets are waiting it sends 50, then another 50
  after 1.1 s, rather than waiting out another interval. At one bucket per minute this never triggers.
- **Offline means it waits, not fails.** Publishing is skipped while `!Particle.connected()`; buckets
  keep accumulating and the next interval sends what fits. If the outage outlasts the 240-slot ring
  (4 hours), the oldest are overwritten and counted in `dropped`.

## Where the data goes: Supabase

```
device cache -> Particle.publish("activity", csv) -> Particle webhook
             -> Edge Function ingest-activity -> Postgres public.activity_buckets
```

Project `internetbutton` (`jdewkdjgbghtvzwsmpuw`, `us-east-2`). Full schema and rationale in
[`docs/ACTIVITY.md`](../../docs/ACTIVITY.md).

### The webhook

`webhook.json.template` is the config, with `<ANON_KEY>` in place of the project's **publishable**
key. Register it with:

```sh
sed "s|<ANON_KEY>|<publishable-key>|" webhook.json.template > /tmp/activity-webhook.json
particle webhook create /tmp/activity-webhook.json
particle webhook list                    # confirm
particle webhook delete <id>             # remove
```

The publishable key is public by design, which is why it can live in a webhook config — but it is also
the only gate on the endpoint (see below).

### Duplicates are expected, and harmless

The firmware retries any bucket it could not confirm, so **the same bucket will arrive more than
once**. The table has `unique (device_id, bucket_start)` and the function upserts with
`ignoreDuplicates`, so a replay is a no-op. Posting the identical payload twice gives
`inserted: 1` then `inserted: 0, duplicates: 1`.

### Reading it back

```sql
select to_char(bucket_start at time zone 'UTC', 'YYYY-MM-DD HH24:MI') as minute,
       impulses,
       round(impulse_sum::numeric / nullif(impulses, 0), 1) as mean_peak,
       max_delta, max_dz
  from public.activity_buckets
 order by bucket_start desc
 limit 20;
```

`max_delta` and `max_dz` are `NULL` when the firmware sent 0, and **`max_dz = 255` means "at least
255"** because the firmware field is a `uint8`.

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
| Supabase ingest, end to end | Yes — device `flush` moved 5 pending buckets into 5 Postgres rows (02:35–02:39 UTC, 3,833 impulses) |
| Upload cadence | Yes — measured 300 s between batches, 5 buckets each, three consecutive batches |
| Ingest rejects unauthenticated calls | Yes — `HTTP 401` with no `Authorization` header |
| Ingest is idempotent | Yes — replaying the same payload returned `inserted: 0, duplicates: 1` |

## Gotchas

- **`maxDz` saturates.** It is a `uint8`, and a bucket containing a bump hit exactly 255, so any such
  bucket reports a lower bound rather than a value.
- **`flushmin` resets to 5 on every boot**, so `sent` looks stuck for the first 5 minutes after a
  flash. That is the cache working, not a fault.
- **Reading several counters separately is not a snapshot.** `closed` and `held` can appear
  inconsistent simply because they were read either side of a bucket closing.
- **Right after a flash, variable reads 404** for 10–20 s (`"Unable to get variable on device during
  an OTA"`). Wait for a variable that only the new firmware has.
