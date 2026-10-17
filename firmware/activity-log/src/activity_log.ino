/*
 * activity-log — record what the device senses, keep a local cache, and upload it
 * to the cloud in batches.
 *
 * The requirement (docs/ACTIVITY.md): "All data the device records should be
 * stored in a cache related to the local environment. This cache would be
 * periodically uploaded to a 'cloud' location."
 *
 * WHY IT CANNOT STORE RAW DATA
 *
 * There is no filesystem on the Photon (that is Gen 3+), program flash is not
 * writable at runtime, and the only persistent store — emulated EEPROM — is a few
 * kilobytes. At the measured typing rate of ~8.8 impulses/s that is ~760,000
 * records a day, so raw storage is not a question of tuning; it is impossible.
 *
 * So the cache holds FIXED TIME BUCKETS, not impulses. One bucket per wall-clock
 * minute, carrying:
 *
 *     count   how many impulses            (16 bit)
 *     sum     sum of their peak deltas     (32 bit — a mean is sum/count)
 *     max     the largest peak delta       (16 bit)
 *     dz      the largest |dz| in the bucket (8 bit)
 *
 * `dz` is carried because measurement showed the Z axis is what actually picks up
 * a keystroke through the desk (peak 12 vs 4 at rest, while X and Y barely moved).
 * See firmware/motion-pulse/README.md.
 *
 * The cache is RAM-only by design (a decision recorded in docs/ACTIVITY.md):
 * simple, and it holds four hours. EEPROM could persist it across a power loss,
 * but at 4 bytes of flash per byte stored that is only worth doing at a low
 * snapshot rate, so it is left out rather than guessed at.
 *
 * LOSING DATA IS COUNTED, NOT SILENT
 *
 * A ring buffer overwrites its oldest entry when full, and publishing can be
 * refused when offline. Both are losses, and both are counted separately:
 *
 *     dropped   buckets overwritten before they could be published
 *     discards  impulses seen while the clock was still unsynced
 *
 * If these stay at zero, nothing was lost. That is the point of exposing them.
 */

#include "Particle.h"
#include "InternetButton.h"

InternetButton b = InternetButton();

// The accelerometer is a file-scope global in InternetButton.cpp, not a member,
// so it is reachable this way. This is the same finding that made typing
// detection possible: begin() only soft-resets the chip and sets no output data
// rate, leaving it at its ~12.5 Hz power-on default, which filters short impulses
// away before they are digitised.
extern ADXL362 accelerometer;

// ------------------------------------------------------------------ settings

static int  cfgThresh = 3;             // delta that counts as activity (floor is 0-2)
static int  cfgOdr    = 400;           // must be high: a keystroke is a short impulse
static int  cfgFlushMin = 5;           // minutes between upload attempts

// Visual feedback. This firmware's job is to be tested, and a logger with no
// indicator cannot be tested by looking at it: there is no way to tell "detected
// it and stored it" from "detected nothing". So each impulse gives a brief dim
// pulse. It is deliberately subtle and capped low — the ring is an indicator
// here, not the display, and motion-pulse owns the VU-meter behaviour.
static int  cfgLedMax   = 40;          // brightness cap for the indicator
static int  cfgLedDecay = 220;         // ms for the pulse to fade out

static const unsigned long SAMPLE_MS   = 2;        // 500 Hz
static const unsigned long MIN_PUB_GAP = 1100;     // ms; the cloud allows ~1 publish/s
static const int           HIT_DROP    = 3;        // fall from peak that ends an impulse

// 1-minute buckets, 240 of them = four hours of history, 16 bytes each.
static const int      BUCKET_MINUTES = 1;
static const int      RING           = 240;
static const size_t   PAYLOAD_MAX    = 1024;       // cloud event data limit
static const int      BUCKETS_PER_PUB = 50;        // ~12 chars each, so well inside

struct Bucket {
    uint32_t startMin;   // unix minutes at the start of the bucket
    uint32_t sum;        // sum of impulse peaks
    uint16_t count;      // impulses in the bucket
    uint16_t maxDelta;   // largest peak delta
    uint8_t  maxDz;      // largest |dz| seen while active
    uint8_t  pad;
};

static Bucket ring[RING];
static uint16_t ringHead = 0;          // where the next closed bucket goes
static uint16_t ringHeld = 0;          // valid closed buckets in the ring

static uint32_t closedTotal = 0;       // buckets closed since boot (absolute index)
static uint32_t pubIndex    = 0;       // absolute index of the next bucket to publish
static uint32_t dropped     = 0;       // buckets overwritten before they were sent
static uint32_t discards    = 0;       // impulses seen before the clock was valid
static uint32_t sentTotal   = 0;       // buckets successfully published

static Bucket cur;                     // the open bucket
static bool   bucketOpen  = false;

static const uint8_t ODR_FLAGS[6] = {
    XL362_FILTER_FLAG_ODR12,  XL362_FILTER_FLAG_ODR25,  XL362_FILTER_FLAG_ODR50,
    XL362_FILTER_FLAG_ODR100, XL362_FILTER_FLAG_ODR200, XL362_FILTER_FLAG_ODR400
};
static const int ODR_CHOICES[6] = {12, 25, 50, 100, 200, 400};

// -------------------------------------------------------------------- state

static bool haveAccel = false;
static int  ax = 0, ay = 0, az = 0;
static bool inHit  = false;
static int  hitPeak = 0;

static unsigned long lastSampleMs  = 0;
static unsigned long lastPublishMs = 0;
static bool          forceFlush    = false;

// indicator
static float         ledLevel   = 0.0f;
static unsigned long lastLedMs  = 0;
static int           lastPaint  = -1;
static bool          ledEnable  = true;

static void paint(int brightness)
{
    if (brightness < 0)   brightness = 0;
    if (brightness > 255) brightness = 255;
    if (brightness == lastPaint) return;      // avoid rewriting the ring every loop
    lastPaint = brightness;

    if (brightness == 0) { b.allLedsOff(); return; }
    b.allLedsOn(brightness, brightness * 3 / 4, 0);   // amber, dimmed
}

static void serviceLed()
{
    unsigned long now = millis();
    unsigned long dt  = now - lastLedMs;
    lastLedMs = now;
    if (dt == 0 || ledLevel <= 0.0f) return;

    long decayMs = cfgLedDecay > 0 ? cfgLedDecay : 1;
    ledLevel -= (255.0f * (float)dt) / (float)decayMs;
    if (ledLevel < 0.0f) ledLevel = 0.0f;
}

static char payload[PAYLOAD_MAX];
static String cloudLast   = "";
static String cloudBucket = "";

static int cloudThreshold = 3, cloudOdr = 400, cloudFlushMin = 5;
static int cloudHeld = 0, cloudClosed = 0, cloudSent = 0;
static int cloudPending = 0, cloudDropped = 0, cloudDiscards = 0;
static int cloudCount = 0, cloudEeprom = 0, cloudTimeOk = 0;

// Uptime and reset reason. A first 4-minute run rebooted partway through (the
// counters all returned to zero), and a reboot silently destroys the whole
// RAM-only cache — so it matters, and guessing at the cause would be worthless.
// System.resetReason() distinguishes a deliberate reset from a brownout, a panic
// (stack overflow, failed assertion) and so on.
static int cloudUptime = 0, cloudResetReason = 0, cloudResetData = 0;

// ---------------------------------------------------------------------- odr

static bool applyOdr(int hz)
{
    for (int i = 0; i < 6; i++) {
        if (ODR_CHOICES[i] == hz) {
            accelerometer.setOutputDatarate(ODR_FLAGS[i]);
            accelerometer.setBandwidth(XL362_FILTER_FLAG_FBW);        // keep the high frequencies
            accelerometer.setNoiseLevel(XL362_POWER_FLAG_NOISE_NORMAL);
            cfgOdr = hz;
            cloudOdr = hz;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ buckets

static void openBucket(uint32_t startMin)
{
    memset(&cur, 0, sizeof(cur));
    cur.startMin = startMin;
    bucketOpen = true;
}

// The ring only holds the newest `ringHeld` closed buckets, so any unpublished
// bucket older than that has already been overwritten. Advance the publish cursor
// past them and count the loss, rather than publishing a bucket that now holds
// different data.
static void dropUnsendable()
{
    uint32_t oldest = (closedTotal > ringHeld) ? closedTotal - ringHeld : 0;
    if (pubIndex < oldest) {
        dropped += oldest - pubIndex;
        pubIndex = oldest;
    }
}

static void closeBucket()
{
    if (!bucketOpen) return;

    ring[ringHead] = cur;
    ringHead = (ringHead + 1) % RING;
    if (ringHeld < RING) ringHeld++;
    closedTotal++;
    dropUnsendable();
    bucketOpen = false;

    // keep a readable copy of the bucket just closed, so the numbers can be
    // checked without waiting for an upload
    char buf[96];
    snprintf(buf, sizeof(buf), "%lu,%u,%lu,%u,%u",
             (unsigned long)cur.startMin, cur.count,
             (unsigned long)cur.sum, cur.maxDelta, cur.maxDz);
    cloudLast = buf;
}

static void serviceBuckets()
{
    if (!Time.isValid()) return;
    uint32_t minNow = (uint32_t)(Time.now() / 60);

    if (!bucketOpen) { openBucket(minNow); return; }
    if (minNow != cur.startMin) {
        closeBucket();
        // A clock jump can skip minutes. Start the new bucket at the current
        // minute rather than trying to invent buckets for the gap.
        openBucket(minNow);
    }
}

// ------------------------------------------------------------------- motion

static void closeHit()
{
    if (!inHit) return;

    if (bucketOpen) {
        if (cur.count < 65535) cur.count++;
        cur.sum += (uint32_t)hitPeak;
        if (hitPeak > cur.maxDelta) cur.maxDelta = (uint16_t)hitPeak;
    } else {
        discards++;   // clock not synced yet, so it has no place in time
    }

    // feedback: every impulse that is actually recorded gives a visible pulse, so
    // "is it detecting?" is answerable by looking at the device
    if (ledEnable) {
        int want = 10 + hitPeak;
        if (want > cfgLedMax) want = cfgLedMax;
        if ((float)want > ledLevel) ledLevel = (float)want;
    }

    inHit = false;
}

static void sampleMotion()
{
    int x = b.readX(), y = b.readY(), z = b.readZ();
    if (!haveAccel) { ax = x; ay = y; az = z; haveAccel = true; return; }

    int dx = abs(x - ax), dy = abs(y - ay), dz = abs(z - az);
    int d  = dx + dy + dz;
    ax = x; ay = y; az = z;

    // An impulse is one keystroke-like impulse, not one sample: it opens when the
    // signal crosses the threshold and closes when it falls back to the floor or
    // turns over. Measuring per sample instead would report a different number
    // for every ODR.
    if (d > cfgThresh) {
        if (!inHit)                     { inHit = true; hitPeak = d; }
        else if (d > hitPeak)            { hitPeak = d; }
        else if (d + HIT_DROP <= hitPeak) { closeHit(); }

        if (bucketOpen && dz > cur.maxDz) cur.maxDz = (uint8_t)min(dz, 255);
    } else if (inHit) {
        closeHit();
    }
}

// ------------------------------------------------------------------ publish

// Build a compact payload for buckets [pubIndex, pubIndex+n). CSV rather than
// JSON because the event payload is capped at 1024 bytes and every byte spent on
// punctuation is a bucket that does not get uploaded.
static size_t buildPayload(uint32_t from, int n)
{
    uint32_t oldest = (closedTotal > ringHeld) ? closedTotal - ringHeld : 0;
    size_t used = 0;

    used += snprintf(payload + used, PAYLOAD_MAX - used, "v1,%lu,%d,",
                     (unsigned long)ring[from % RING].startMin, n);
    if (used >= PAYLOAD_MAX) return 0;

    int written = 0;
    for (int i = 0; i < n && written < n; i++) {
        uint32_t absIdx = from + i;
        if (absIdx < oldest || absIdx >= closedTotal) break;
        Bucket &bk = ring[absIdx % RING];

        int w = snprintf(payload + used, PAYLOAD_MAX - used, "%u,%lu,%u,%u;",
                         bk.count, (unsigned long)bk.sum, bk.maxDelta, bk.maxDz);
        if (w < 0) break;
        if ((size_t)w >= PAYLOAD_MAX - used) break;   // would not fit; stop cleanly
        used += (size_t)w;
        written++;
    }
    return (size_t)written;
}

static void servicePublish()
{
    if (pubIndex >= closedTotal) { forceFlush = false; return; }

    unsigned long now = millis();
    bool due = forceFlush || (now - lastPublishMs >= (unsigned long)cfgFlushMin * 60000UL);
    if (!due) return;
    if (now - lastPublishMs < MIN_PUB_GAP) return;   // never exceed the cloud rate limit
    if (!Particle.connected()) return;               // keep it cached until it is

    int n = (int)min((uint32_t)BUCKETS_PER_PUB, closedTotal - pubIndex);
    size_t got = buildPayload(pubIndex, n);
    if (got == 0) { pubIndex++; dropped++; return; }   // unbuildable, cannot ever send

    lastPublishMs = now;

    // Only advance if the publish was accepted. If the cloud refuses it, the
    // bucket stays pending and is retried — that is the whole point of a cache.
    if (Particle.publish("activity", payload, PRIVATE)) {
        pubIndex += (uint32_t)got;
        sentTotal += (uint32_t)got;
    }
}

// ---------------------------------------------------------------- functions

int fnThresh(String a)
{
    a.trim();
    if (a.length()) { int v = a.toInt(); if (v >= 1 && v <= 255) cfgThresh = v; }
    cloudThreshold = cfgThresh;
    return cfgThresh;
}

int fnFlushMin(String a)
{
    a.trim();
    if (a.length()) { int v = a.toInt(); if (v >= 1 && v <= 60) cfgFlushMin = v; }
    cloudFlushMin = cfgFlushMin;
    return cfgFlushMin;
}

int fnOdr(String a)
{
    a.trim();
    if (a.length()) applyOdr(a.toInt());
    return cfgOdr;
}

// "0" silences the indicator, "1" (or blank) enables it. Returns the state.
int fnLed(String a)
{
    a.trim();
    if (a.length()) ledEnable = (a.toInt() != 0);
    if (!ledEnable) { ledLevel = 0.0f; paint(0); }
    return ledEnable ? 1 : 0;
}

// publish everything pending as fast as the rate limit allows
int fnFlush(String)
{
    forceFlush = true;
    return (int)(closedTotal - pubIndex);
}

int fnReset(String)
{
    ringHead = 0; ringHeld = 0;
    closedTotal = 0; pubIndex = 0; sentTotal = 0;
    dropped = 0; discards = 0;
    bucketOpen = false;
    cloudLast = "";
    return 1;
}

// "get <back>" — load a closed bucket into the `bucket` variable, 1 = most recent.
// Device OS 2.3.1 only supports int-returning cloud functions, so the CSV itself
// has to travel as a String variable; this function only selects which one.
int fnGet(String a)
{
    a.trim();
    int back = a.length() ? a.toInt() : 1;
    if (back < 1 || (uint16_t)back > ringHeld) { cloudBucket = "none"; return -1; }

    uint32_t absIdx = closedTotal - (uint32_t)back;
    uint32_t oldest = closedTotal - ringHeld;
    if (absIdx < oldest) { cloudBucket = "gone"; return -2; }

    Bucket &bk = ring[absIdx % RING];
    char buf[96];
    snprintf(buf, sizeof(buf), "%lu,%u,%lu,%u,%u",
             (unsigned long)bk.startMin, bk.count,
             (unsigned long)bk.sum, bk.maxDelta, bk.maxDz);
    cloudBucket = buf;
    return back;
}

// ------------------------------------------------------------------- cloud

static void updateCloud()
{
    cloudHeld     = ringHeld;
    cloudClosed   = (int)closedTotal;
    cloudSent     = (int)sentTotal;
    cloudPending  = (int)(closedTotal - pubIndex);
    cloudDropped  = (int)dropped;
    cloudDiscards = (int)discards;
    cloudCount    = bucketOpen ? (int)cur.count : 0;
    cloudTimeOk   = Time.isValid() ? 1 : 0;
    cloudOdr      = cfgOdr;
    cloudUptime   = (int)(millis() / 1000UL);
}

// ------------------------------------------------------------------- setup

void setup()
{
    b.begin();
    b.setBrightness(64);

    // after b.begin(), which soft-resets the accelerometer
    applyOdr(cfgOdr);
    lastLedMs = millis();

    // Measured, not assumed: docs/ACTIVITY.md quotes ~3.5 KB for the Photon's
    // emulated EEPROM but flags it as a quoted figure. This reports the real
    // value from the device. Measured: 2048.
    cloudEeprom = (int)EEPROM.length();

    cloudResetReason = (int)System.resetReason();
    cloudResetData   = (int)System.resetReasonData();

    Particle.variable("thresh",   cloudThreshold);
    Particle.variable("odr",      cloudOdr);
    Particle.variable("flushmin", cloudFlushMin);
    Particle.variable("held",     cloudHeld);      // closed buckets in the ring
    Particle.variable("closed",   cloudClosed);    // buckets closed since boot
    Particle.variable("sent",     cloudSent);      // buckets uploaded
    Particle.variable("pending",  cloudPending);   // closed but not yet uploaded
    Particle.variable("dropped",  cloudDropped);   // overwritten before upload
    Particle.variable("discards", cloudDiscards);  // impulses before clock sync
    Particle.variable("count",    cloudCount);     // impulses in the open bucket
    Particle.variable("timeok",   cloudTimeOk);
    Particle.variable("eepromlen", cloudEeprom);
    Particle.variable("uptime",   cloudUptime);       // seconds since boot
    Particle.variable("reset",    cloudResetReason);  // why the device last restarted
    Particle.variable("resetdata", cloudResetData);
    Particle.variable("last",     cloudLast);      // most recent closed bucket
    Particle.variable("bucket",   cloudBucket);    // selected by `get`

    Particle.function("get",      fnGet);
    Particle.function("flush",    fnFlush);
    Particle.function("reset",    fnReset);
    Particle.function("thresh",   fnThresh);
    Particle.function("flushmin", fnFlushMin);
    Particle.function("odr",      fnOdr);
    Particle.function("led",      fnLed);

    b.allLedsOff();
    lastPublishMs = millis();
}

// -------------------------------------------------------------------- loop

void loop()
{
    unsigned long now = millis();

    if (now - lastSampleMs >= SAMPLE_MS) {
        lastSampleMs = now;
        sampleMotion();
    }

    serviceBuckets();
    servicePublish();
    serviceLed();
    paint((int)ledLevel);
    updateCloud();
    delay(1);
}
