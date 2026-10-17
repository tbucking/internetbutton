# Activity

## Requirement

All data the device records should be stored in a cache related to the local environment. This cache
would be periodically uploaded to a "cloud" location.

---

## What the hardware actually allows

These constraints decide the design, so they come first. Verified from the Device OS 2.3.1 sources
(`services/inc/eeprom_emulation.h`, `wiring/inc/spark_wiring_eeprom.h`).

**There is no filesystem.** `Particle.filesystem` (LittleFS) is Gen 3 and later. The Photon is Gen 2,
so there is no way to append records to a file.

**Flash cannot be written at runtime.** The Photon has ~1 MB of flash and this firmware uses ~16 KB of
it, but program flash is not writable by user code. That free space is not usable for data.

**EEPROM is the only persistent store, and it is small.** `EEPROM` on the Photon is *emulated* in
flash:

- Each byte written becomes a **4-byte record** (1 data + 1 status + 2 index) appended to the active
  page.
- Two flash sectors are used, alternating. Writing a full page triggers a **page swap**: erase the
  alternate sector, copy valid records, switch. That erase blocks the application for hundreds of ms.
- Capacity is `smallest page size / 4 / 2` — i.e. **half** the theoretical maximum.

**Measured, not quoted: `EEPROM.length()` reports 2048 bytes on this device.** The commonly quoted
figure for the Photon is 3.5 KB; that is wrong here by 41%, which is why the firmware exposes
`eepromlen` and reports the real value. Because capacity is
`page / 4 / 2 = 2048`, the underlying page size is `2048 × 8 = 16384`, i.e. **16 KB**, with a page
swap after ~4096 stored bytes.

### The consequence

The device **cannot store raw activity data**. A keystroke produces an impulse, and a measured typing
bucket held **948 impulses in one minute** (section "First run" below). One day at that rate is
~1.4 million impulses. Against 2048 bytes of EEPROM that is not a tuning problem, it is off by three
orders of magnitude.

So the cache must be **aggregated**, not raw: fixed time buckets carrying a count, a sum and a max.

A bucket is 16 bytes (`startMin` 4, `sum` 4, `count` 2, `maxDelta` 2, `maxDz` 1, padding 1):

| Bucket size | Buckets/day | Bytes/day | Retention in 2048 B | Retention in RAM ring (240) |
| --- | --- | --- | --- | --- |
| 1 minute | 1,440 | ~23 KB | **~2.1 hours** | **4 hours** |
| 5 minutes | 288 | ~4.6 KB | ~11 hours | 20 hours |
| 1 hour | 24 | ~384 B | ~5.3 days | 10 days |

**Chosen: 1-minute buckets, RAM-only.** The binding limit is the 240-slot RAM ring, giving 4 hours of
history; EEPROM would hold only 2.1 hours, so it is not worth the wear. RAM is plentiful — the Photon
has 128 KB and this firmware's static footprint is ~6.6 KB, most of it the ring and payload buffer.

**Upload must be batched anyway.** Particle Cloud allows roughly one `Particle.publish` per second and
caps a single event payload at 1024 bytes, so per-impulse publishing is not an option even if storage
were unlimited. The implementation sends up to 50 buckets per event and never exceeds the rate limit.

### Wear estimate, if EEPROM were used

Each stored byte costs a 4-byte record, so a 16 KB page absorbs ~4096 stored bytes before a page swap.
A 16-byte bucket therefore costs 16 records, or **~256 bucket-writes per page swap**. At one bucket per
minute that is a swap every ~4.3 hours, or ~2000 swaps/year — inside typical flash endurance (order
10⁴ cycles) for several years.

That is an estimate derived from the capacity equation, not a measurement, and it assumes the erase
count is the only wear. It is affordable **at low frequency**; it is not affordable per impulse.

---

## What was measured on the device

### First run (RAM-only cache, 1-minute buckets)

Three buckets uploaded in three consecutive minutes:

| bucket | count | mean peak (sum/count) | max | max dz |
| --- | --- | --- | --- | --- |
| `29870048` | 286 | 4.4 | 12 | 10 |
| `29870049` | 948 | 7.6 | 263 | 255 |
| `29870050` | 755 | 4.6 | 14 | 10 |

Two things worth reading off this:

- `maxDz` is a `uint8` and bucket 2 hit **255**, so it saturated. Any bucket containing a bump (that
  `max = 263` is the board being moved, not typing) makes `maxDz` a lower bound rather than a value.
  This should be widened before relying on it.
- `count` is *impulses*, not samples. Measuring per sample would report a different number for every
  ODR, so an impulse opens above threshold and closes when it falls back to the floor or turns over.

### A reboot destroyed the cache, as the design implies

Partway through that first run the counters went from `closed=3 sent=3` to **all zero** — the device
rebooted. Nothing had been unsent, so no data was actually lost that time, but the whole 3-bucket
history was gone. This is the RAM-only tradeoff happening for real, not in theory: **any reboot loses
every bucket not yet uploaded.**

The cause is **not known**. `resetReason` now reports `70` (`RESET_REASON_SOFTWARE`), but that value
was read after an OTA flash, which plausibly explains it on its own. A subsequent 3-minute watch showed
uptime climbing monotonically (9 s → 195 s) with no repeat. `uptime` and `reset`/`resetdata` are
exposed so a recurrence is diagnosable rather than invisible.

If reboots turn out to be routine, the RAM-only decision is the thing to revisit — EEPROM persistence
would cost a page swap roughly every 4 hours, which is affordable.


---

## Decisions taken

1. **Cloud = Particle Cloud events.** The device publishes `activity` events; something must consume
   them (a webhook to an endpoint you control, or a subscriber). Particle Cloud is a stream, not a
   store, so persistence at the far end is still an open piece.
2. **A record is an aggregated 1-minute bucket:** count, sum, max delta, max |dz|.
3. **RAM only.** No EEPROM snapshot, so a reboot loses all unuploaded buckets (observed once).
4. **Cache is on the device**, not on the local network.

### Still open

- **What consumes the events?** Without a webhook or a subscriber the data is published and dropped.
- **Is `maxDz` worth keeping?** It saturated at 255 in the first run; widen it or drop it.
- **Should the cache survive reboots?** Revisit if the reboot seen in the first run turns out to be
  recurring.