/*
 * motion-pulse — all 11 LEDs pulse on any accelerometer activity, with brightness
 * proportional to how hard the board was moved.
 *
 * A VU meter for motion: still = dark, a light tap = a dim glow, a hard shake =
 * full brightness. The whole ring moves together as one indicator.
 *
 * BUILD:
 *   ../../build.sh motion-pulse --flash      (applies --target 2.3.1 and verifies)
 * or by hand with --target 2.3.1. Never compile a Photon without it — see
 * docs/INIT.md section 6.
 *
 * HOW BRIGHTNESS MAPS TO SEVERITY, AND WHY IT IS TUNABLE
 *
 * "Severity" is the frame-to-frame change in the raw accelerometer reading:
 *
 *     delta = |x - lastX| + |y - lastY| + |z - lastZ|
 *
 * summed across the three axes, in 8-bit ADXL362 LSB. At rest the measured floor
 * is 0-2 (see the README); a tap is tens, a hard shake is hundreds.
 *
 * That range is wide and non-obvious, so the mapping is deliberately NOT baked
 * in: `gain` scales delta to a target brightness and `max` caps it. Both are
 * settable from the cloud without recompiling, so the response can be tuned
 * against how it actually feels rather than how it was guessed. `peak` and
 * `delta` are exposed so the numbers can be read while you do it.
 *
 *     target = clamp(delta * gain / 16, floorWhenActive, max)
 *
 * The envelope is the "pulse": brightness jumps straight to the target on
 * activity (instant attack, so a short tap is not missed), then fades linearly to
 * zero over `decay` ms. Continual motion keeps re-triggering, so shaking holds it
 * bright.
 *
 * THRESHOLD. `thresh` exists only to sit just above the noise floor. Anything
 * above it counts as activity, including a gentle nudge — "any kind of activity"
 * is the requirement, so it is set low (default 3) and the *brightness* carries
 * the magnitude. Raising it makes the ring harder to trigger but not brighter.
 *
 * POWER. Brightness is capped (`max`, default 150 of 255) and the library's ring
 * brightness is left at 255 so the envelope has full control. All 11 WS2812B at
 * RGB 255 is ~660 mA, which is the load that mattered in docs/INIT.md section 12,
 * so the cap is deliberate. Raise it knowingly.
 */

#include "InternetButton.h"

InternetButton b = InternetButton();

// The ADXL362 is a file-scope global inside InternetButton.cpp and is NOT a
// member of InternetButton, so it is reachable via extern. That matters a lot
// here: setOutputDatarate/setBandwidth live on the accelerometer, and the
// wrapper never calls them.
//
// `begin()` does a soft reset ONLY — it sets no range, bandwidth or rate, so the
// chip comes up on its power-on default of about 12.5 Hz. Detecting a keypress
// needs the opposite: a keystroke is a sharp, high-frequency impulse, and at
// 12.5 Hz the sensor's own anti-alias filter smooths it away before it is ever
// digitised. No amount of gain recovers information the sensor never captured,
// so the rate has to be raised at the source. This is the single most important
// setting for typing detection.
extern ADXL362 accelerometer;

// ------------------------------------------------------------------ settings

static int  cfgThresh = 3;      // above the measured at-rest floor (0-2)
static int  cfgGain   = 16;     // delta -> brightness scaling
static int  cfgMax    = 150;    // brightness cap (of 255)
static int  cfgFloor  = 12;     // minimum brightness for any detected activity
static long cfgDecay  = 650;    // ms to fade from full to dark
static int  cfgR = 255, cfgG = 140, cfgB = 0;   // amber, like a VU meter

static const unsigned long SAMPLE_MS = 2;       // 500 Hz — must exceed the ODR
static const long          MAX_DECAY  = 60000;  // clamp for the cloud-set value

// Output data rate, in Hz. Measured, not assumed: this is the dominant factor for
// detecting brief impulses like a keystroke. Settable from the cloud so 12.5 vs
// 400 can be compared on the same typing without a reflash.
static int cfgOdr = 400;

static const int ODR_CHOICES[6] = {12, 25, 50, 100, 200, 400};
static const uint8_t ODR_FLAGS[6] = {
    XL362_FILTER_FLAG_ODR12,  XL362_FILTER_FLAG_ODR25,  XL362_FILTER_FLAG_ODR50,
    XL362_FILTER_FLAG_ODR100, XL362_FILTER_FLAG_ODR200, XL362_FILTER_FLAG_ODR400
};

static bool applyOdr(int hz)
{
    for (int i = 0; i < 6; i++) {
        if (ODR_CHOICES[i] == hz) {
            accelerometer.setOutputDatarate(ODR_FLAGS[i]);
            // full bandwidth = pass the highest frequencies the rate allows;
            // half bandwidth low-passes, which is the opposite of what is wanted
            accelerometer.setBandwidth(XL362_FILTER_FLAG_FBW);
            // NORMAL noise is the lowest-noise option, so the weakest impulses
            // still stand out above the floor
            accelerometer.setNoiseLevel(XL362_POWER_FLAG_NOISE_NORMAL);
            cfgOdr = hz;
            return true;
        }
    }
    return false;
}

// --------------------------------------------------------------------- state

static bool haveAccel = false;
static int  ax = 0, ay = 0, az = 0;

static int  delta     = 0;      // this sample's motion magnitude
static int  peakDelta = 0;      // highest since reset
static int  peakLevel = 0;      // highest brightness since reset
static unsigned long episodes = 0;

// Per-axis change, and the peak of each since reset. The reason to care: delta
// sums |dx|+|dy|+|dz|, so it carries three times the sensor noise. If a keystroke
// arrives mostly through one axis (plausibly Z, down through the desk), summing
// all three dilutes it. These make that measurable instead of assumed.
static int  dxs = 0, dys = 0, dzs = 0;
static int  pDx = 0, pDy = 0, pDz = 0;

static float level      = 0.0f; // current brightness, 0..max
static unsigned long lastSampleMs = 0;
static unsigned long lastDecayMs  = 0;
static unsigned long lastEpisodeMs = 0;

static const unsigned long EPISODE_GAP_MS = 400;   // separate events for counting

static int  lastR = -1, lastG = -1, lastB = -1;    // avoid redundant ring writes

// ---------------------------------------------------------------- histogram
//
// `peak` answers "did it detect anything", never "did it detect everything". To
// judge typing coverage we need the distribution of individual impulses, so each
// one is binned by its peak magnitude.
//
// An impulse is closed either when the signal falls back to the floor, or when it
// has turned over — dropped by HIT_DROP from its own peak. The second case is what
// stops a continuous stream of keystrokes collapsing into one giant "hit": at
// 400 ms per episode gap, a sentence is a single event, so episode counting cannot
// see individual keys. Peak-turning needs no gap at all.
//
// Both paths bin the peak, so counts are per impulse, not per sample. Bin 0 is
// [0,4) — the at-rest noise floor — so a baseline run reports directly how many
// phantom hits to expect before any real typing is counted.

static const int HBINS = 13;
static const int HEDGE[HBINS] = {0, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256};
static const int HIT_DROP = 3;      // delta fall from peak that closes an impulse

static unsigned int hist[HBINS];
static unsigned int hits   = 0;
static bool captureOn      = true;
static bool inHit          = false;
static int  hitPeak        = 0;

static void binHit(int pk)
{
    int i = 0;
    while (i < HBINS - 1 && pk >= HEDGE[i + 1]) i++;
    if (hist[i] < 65535) hist[i]++;
    if (hits   < 65535) hits++;
}

// --------------------------------------------------------------- cloud state

static String cloudColour = "";
static int    cloudDelta  = 0;
static int    cloudLevel  = 0;
static int    cloudPeak   = 0;
static int    cloudEvents = 0;
static int    cloudGain   = 16;
static int    cloudMax    = 150;
static int    cloudThresh = 3;
static int    cloudDecay  = 650;
static int    cloudOdr    = 400;
static int    cloudHits   = 0;
static int    cloudPdx    = 0, cloudPdy = 0, cloudPdz = 0;
static String cloudHist   = "";
static unsigned long lastHistMs = 0;

static void clearHist()
{
    for (int i = 0; i < HBINS; i++) hist[i] = 0;
    hits = 0; inHit = false; hitPeak = 0;
}

// --------------------------------------------------------------------- output

static void paint(int brightness)
{
    if (brightness < 0) brightness = 0;
    if (brightness > 255) brightness = 255;

    // scale the base colour by the brightness, so the envelope controls the
    // output directly
    int r = (int)cfgR * brightness / 255;
    int g = (int)cfgG * brightness / 255;
    int bl = (int)cfgB * brightness / 255;

    if (r == lastR && g == lastG && bl == lastB) return;   // nothing changed
    lastR = r; lastG = g; lastB = bl;

    b.allLedsOn((uint8_t)r, (uint8_t)g, (uint8_t)bl);
}

// --------------------------------------------------------------------- motion

static void sampleMotion()
{
    int x = b.readX(), y = b.readY(), z = b.readZ();

    if (!haveAccel) { ax = x; ay = y; az = z; haveAccel = true; return; }

    dxs = abs(x - ax); dys = abs(y - ay); dzs = abs(z - az);
    int d = dxs + dys + dzs;
    ax = x; ay = y; az = z;
    delta = d;

    if (d   > peakDelta) peakDelta = d;
    if (dxs > pDx) pDx = dxs;
    if (dys > pDy) pDy = dys;
    if (dzs > pDz) pDz = dzs;

    // impulse binning — see the histogram section
    if (captureOn) {
        if (d > cfgThresh) {
            if (!inHit)                       { inHit = true; hitPeak = d; }
            else if (d > hitPeak)               { hitPeak = d; }
            else if (d + HIT_DROP <= hitPeak)   { binHit(hitPeak); inHit = false; }
        } else if (inHit) {
            binHit(hitPeak);
            inHit = false;
        }
    }

    if (d > cfgThresh) {
        // severity -> brightness, with a floor so even a nudge is visible
        int target = d * cfgGain / 16;
        if (target < cfgFloor) target = cfgFloor;
        if (target > cfgMax)   target = cfgMax;

        // instant attack; a later sample within the same episode can raise it
        if ((float)target > level) level = (float)target;

        if (millis() - lastEpisodeMs > EPISODE_GAP_MS) {
            lastEpisodeMs = millis();
            episodes++;
            if ((int)level > peakLevel) peakLevel = (int)level;
        }
    }
}

static void decayEnvelope()
{
    unsigned long now = millis();
    unsigned long dt  = now - lastDecayMs;
    lastDecayMs = now;
    if (dt == 0 || level <= 0.0f) return;

    long decayMs = cfgDecay > 0 ? cfgDecay : 1;
    level -= (255.0f * (float)dt) / (float)decayMs;
    if (level < 0.0f) level = 0.0f;
}

// -------------------------------------------------------------------- cloud

static void updateCloud()
{
    if (cloudDelta != delta)     cloudDelta = delta;
    if (cloudLevel != (int)level) cloudLevel = (int)level;
    if (cloudPeak  != peakDelta) cloudPeak  = peakDelta;
    if (cloudEvents != (int)episodes) cloudEvents = (int)episodes;
    if (cloudGain  != cfgGain)   cloudGain  = cfgGain;
    if (cloudMax   != cfgMax)    cloudMax   = cfgMax;
    if (cloudThresh != cfgThresh) cloudThresh = cfgThresh;
    if (cloudDecay != (int)cfgDecay) cloudDecay = (int)cfgDecay;
    if (cloudOdr   != cfgOdr)        cloudOdr   = cfgOdr;
    if (cloudHits  != (int)hits)     cloudHits  = (int)hits;

    if (cloudPdx != pDx) cloudPdx = pDx;
    if (cloudPdy != pDy) cloudPdy = pDy;
    if (cloudPdz != pDz) cloudPdz = pDz;

    // Rebuild the bin string at most once a second. It is read by the cloud, not
    // sampled per frame, so maintaining it eagerly every loop would be wasted work.
    unsigned long now = millis();
    if (now - lastHistMs >= 1000) {
        lastHistMs = now;
        char buf[128];
        size_t n = 0;
        for (int i = 0; i < HBINS && n < sizeof(buf); i++) {
            int w = snprintf(buf + n, sizeof(buf) - n, i ? ",%u" : "%u", hist[i]);
            if (w < 0) break;
            n += (size_t)w;
        }
        buf[sizeof(buf) - 1] = '\0';
        if (!cloudHist.equals(buf)) cloudHist = buf;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d,%d,%d", cfgR, cfgG, cfgB);
    if (!cloudColour.equals(buf)) cloudColour = buf;
}

int fnGain(String a)   { a.trim(); if (a.length()) { int v = a.toInt(); if (v >= 1 && v <= 128) cfgGain = v; } return cfgGain; }
int fnMax(String a)    { a.trim(); if (a.length()) { int v = a.toInt(); if (v >= 1 && v <= 255) cfgMax = v; } return cfgMax; }
int fnThresh(String a) { a.trim(); if (a.length()) { int v = a.toInt(); if (v >= 1 && v <= 255) cfgThresh = v; } return cfgThresh; }
int fnDecay(String a)  { a.trim(); if (a.length()) { long v = a.toInt(); if (v >= 20 && v <= MAX_DECAY) cfgDecay = v; } return (int)cfgDecay; }
int fnFloor(String a)  { a.trim(); if (a.length()) { int v = a.toInt(); if (v >= 0 && v <= 255) cfgFloor = v; } return cfgFloor; }

// "r,g,b"
int fnColour(String arg)
{
    arg.trim();
    if (!arg.length()) return -1;
    int vals[3] = { cfgR, cfgG, cfgB };
    int idx = 0;
    while (arg.length() && idx < 3) {
        int comma = arg.indexOf(',');
        String tok = (comma < 0) ? arg : arg.substring(0, comma);
        arg = (comma < 0) ? String("") : arg.substring(comma + 1);
        tok.trim();
        if (tok.length()) {
            int v = tok.toInt();
            if (v < 0) v = 0; if (v > 255) v = 255;
            vals[idx] = v;
        }
        idx++;
    }
    cfgR = vals[0]; cfgG = vals[1]; cfgB = vals[2];
    lastR = lastG = lastB = -1;      // force a repaint
    return 1;
}

int fnReset(String) { peakDelta = 0; peakLevel = 0; episodes = 0; clearHist(); return 1; }

// "clear" zeroes the histogram and resumes capture; "0"/"1" pause/resume it.
// Returns 1 when capture is on. Clearing is separate from `reset` so the
// histogram can be re-zeroed without disturbing the rest of the state.
int fnCapture(String a)
{
    a.trim();
    if (a.equalsIgnoreCase("clear")) {
        clearHist();
        // clear the peaks too, so what is read afterwards describes THIS window
        // and not the highest impulse since boot
        peakDelta = 0; peakLevel = 0; episodes = 0;
        pDx = 0; pDy = 0; pDz = 0;
        captureOn = true;
    }
    else if (a.length()) { captureOn = (a.toInt() != 0); }
    return captureOn ? 1 : 0;
}

// 12 | 25 | 50 | 100 | 200 | 400 — blank reports the current rate.
// Reported back so a rejected value is visible rather than silent.
int fnOdr(String a)
{
    a.trim();
    if (a.length()) applyOdr(a.toInt());
    return cfgOdr;
}

// flash the full brightness cap once, to check the ring without moving anything
int fnTest(String)
{
    level = (float)cfgMax;
    lastR = lastG = lastB = -1;      // force a repaint
    paint(cfgMax);
    delay(250);
    // leave the envelope to decay it naturally
    return cfgMax;
}

// --------------------------------------------------------------------- setup

void setup()
{
    b.begin();                       // Internet Button — not begin(1)
    b.setBrightness(255);            // envelope owns the brightness

    // must come AFTER b.begin(), which soft-resets the accelerometer and would
    // otherwise wipe these settings back to the 12.5 Hz power-on default
    applyOdr(cfgOdr);

    Particle.variable("delta",  cloudDelta);   // last sample's magnitude
    Particle.variable("level",  cloudLevel);   // current brightness
    Particle.variable("peak",   cloudPeak);    // highest delta since reset
    Particle.variable("events", cloudEvents);  // activity episodes
    Particle.variable("gain",   cloudGain);
    Particle.variable("max",    cloudMax);
    Particle.variable("thresh", cloudThresh);
    Particle.variable("decay",  cloudDecay);
    Particle.variable("colour", cloudColour);
    Particle.variable("odr",    cloudOdr);
    Particle.variable("hits",   cloudHits);    // impulses binned
    Particle.variable("hist",   cloudHist);    // 13 comma-separated bins
    Particle.variable("pdx",    cloudPdx);     // per-axis peaks since clear
    Particle.variable("pdy",    cloudPdy);
    Particle.variable("pdz",    cloudPdz);

    Particle.function("gain",   fnGain);
    Particle.function("max",    fnMax);
    Particle.function("thresh", fnThresh);
    Particle.function("decay",  fnDecay);
    Particle.function("floor",  fnFloor);
    Particle.function("colour", fnColour);
    Particle.function("reset",  fnReset);
    Particle.function("test",   fnTest);
    Particle.function("odr",    fnOdr);
    Particle.function("capture", fnCapture);

    b.allLedsOff();
    lastDecayMs = millis();
}

// ---------------------------------------------------------------------- loop

void loop()
{
    unsigned long now = millis();

    if (now - lastSampleMs >= SAMPLE_MS) {
        lastSampleMs = now;
        sampleMotion();
    }

    decayEnvelope();
    paint((int)level);
    updateCloud();
    delay(4);
}
