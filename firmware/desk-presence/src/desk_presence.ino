/*
 * desk-presence — reports whether the desk has been disturbed recently, and
 * publishes a cloud event when that changes.
 *
 * WHAT THIS ACTUALLY MEASURES. Be clear about this, because it is easy to
 * oversell: the accelerometer detects *mechanical motion of the board*, not a
 * person. Someone sitting still and reading is invisible to it. So "present"
 * means "something moved this board within the timeout", which is a proxy for
 * presence with known false-away cases — reading, thinking, a phone call. It is
 * a good proxy for "is this desk being used" and a poor one for "is a human
 * within two metres".
 *
 * The state is worthless on its own; what makes it useful is the PUBLISH on
 * transition, which is what a Particle webhook can act on. Device OS 2.3.1 has
 * an old TLS stack and cannot reliably call modern HTTPS endpoints itself, but
 * Particle's servers can — so the device emits a CoAP event and the cloud makes
 * the outbound HTTPS call. That indirection is the whole reason this is
 * practical on a Photon.
 *
 * BUILD TARGET — do not omit this:
 *   particle compile photon . --target 2.3.1 --saveTo desk-presence.bin
 * The Photon tops out at Device OS 2.3.1, and a binary built for a newer version
 * makes the cloud Safe Mode healer replace the device's firmware over the air
 * (docs/INIT.md sections 6 and 11). Verify with
 *   particle binary inspect desk-presence.bin     -> want "2302"
 *
 * BUTTONS (silkscreen is reversed — see docs/INIT.md section 8):
 *   button 1 (silk "4")  ping: declare presence now ("I'm here")
 *   button 2 (silk "3")  toggle the LED indicators (dark mode)
 *   button 3 (silk "2")  reset the activity counters
 *   button 4 (silk "1")  publish the current state immediately
 *
 * POWER. Without a hardware interrupt this polls the accelerometer every 100 ms,
 * so the MCU never sleeps and the Wi-Fi radio stays up. That is fine on USB and
 * wasteful on a battery. The ADXL362 can do this detection autonomously at about
 * 2 uA and wake the MCU on its INT pin, but whether the shield routes INT1/INT2
 * to a Photon GPIO is unverified — see the README.
 */

#include "InternetButton.h"

InternetButton b = InternetButton();

// ------------------------------------------------------------------ settings

static long cfgTimeout = 600;     // seconds without motion before "away"
static int  cfgThresh  = 15;      // motion delta threshold (calibrated; at-rest max measured 2)
static int  cfgSamples = 3;       // consecutive samples to count as real motion

static bool indicatorsOn = true;

static const unsigned long ACCEL_MS     = 100;
static const unsigned long COOLDOWN_MS  = 2000;   // one episode = one event
static const unsigned long PUBLISH_MIN_MS = 5000; // never publish faster than this
static const unsigned long FLASH_MS     = 250;
static const uint8_t       RING_BRIGHTNESS = 60;

// --------------------------------------------------------------------- state

static bool presenceIsPresent = false;             // false => away
static unsigned long lastMotionMs  = 0;
static unsigned long lastEpisodeMs = 0;
static unsigned long lastAccelMs   = 0;
static unsigned long lastPublishMs = 0;
static unsigned long flashEnd      = 0;

static bool haveAccel = false;
static int  ax = 0, ay = 0, az = 0;
static int  moveRun   = 0;

static unsigned long events      = 0;   // motion episodes since boot
static unsigned long transitions = 0;   // present<->away changes
static unsigned long published   = 0;   // publishes ACCEPTED by the cloud

// A transition that could not be published yet (e.g. the cloud was briefly
// unreachable) is retried rather than dropped — losing a transition would make
// the published history disagree with the device's own counters.
static bool         pendingPublish = false;
static const char  *pendingData    = "away";

static unsigned long lastBtnMs = 0;
static int           lastMask  = 0;

// --------------------------------------------------------------- cloud state

static String cloudPresence = "away";
static int    cloudLast     = 0;      // seconds since the last motion
static int    cloudEvents   = 0;
static int    cloudTrans    = 0;
static int    cloudMove     = 0;      // last movement delta (diagnostic)
static int    cloudTimeout  = 600;
static int    cloudInd      = 1;
static int    cloudPublished = 0;     // successful publishes
static int    cloudPending   = 0;     // 1 while a publish is queued/retrying

// ------------------------------------------------------------------ publish

static void requestPublish(const char *data)
{
    pendingData    = data;
    pendingPublish = true;
}

static void servicePublish()
{
    if (!pendingPublish) return;
    if (millis() - lastPublishMs < PUBLISH_MIN_MS) return;
    if (!Particle.connected()) return;

    // PRIVATE: not visible in the public event stream.
    if (Particle.publish("presence", pendingData, 60, PRIVATE)) {
        lastPublishMs  = millis();
        pendingPublish = false;
        published++;
    }
}

// ------------------------------------------------------------------- motion

static void checkMotion()
{
    if (millis() - lastAccelMs < ACCEL_MS) return;
    lastAccelMs = millis();

    int x = b.readX(), y = b.readY(), z = b.readZ();
    if (!haveAccel) { ax = x; ay = y; az = z; haveAccel = true; return; }

    int d = abs(x - ax) + abs(y - ay) + abs(z - az);
    ax = x; ay = y; az = z;
    cloudMove = d;

    if (d > cfgThresh) moveRun++; else moveRun = 0;

    // Requiring several consecutive samples is what rejects an isolated spike:
    // one 40 was seen at rest during calibration, against a floor of 2.
    if (moveRun >= cfgSamples) {
        moveRun = 0;
        lastMotionMs = millis();

        if (millis() - lastEpisodeMs > COOLDOWN_MS) {
            lastEpisodeMs = millis();
            events++;
            if (indicatorsOn) flashEnd = millis() + FLASH_MS;
        }
    }
}

// ----------------------------------------------------------------- presence

static void updatePresence()
{
    unsigned long idle = (millis() - lastMotionMs) / 1000UL;

    // Clamp so the cloud variable can never wrap into a negative int.
    cloudLast = (idle > 86400UL) ? 86400 : (int)idle;

    bool nextPresent = (idle < (unsigned long)cfgTimeout);
    if (nextPresent != presenceIsPresent) {
        presenceIsPresent = nextPresent;
        transitions++;
        requestPublish(nextPresent ? "present" : "away");
    }
}

static void pingPresence()
{
    lastMotionMs = millis();
    if (!presenceIsPresent) {
        presenceIsPresent = true;
        transitions++;
        requestPublish("present");
    }
}

// ---------------------------------------------------------------- indicators

// Deliberately quiet: this sits on a desk and must not be a light source in the
// corner of your eye. "Away" is never fully dark, so an idle monitor is still
// distinguishable from an unpowered or crashed one.
static void render()
{
    static long lastKey = -2;

    if (indicatorsOn && (long)(millis() - flashEnd) < 0) {
        if (lastKey == -1) return;
        lastKey = -1;
        // brief, low-level ring flash: tactile feedback that motion registered
        b.allLedsOn(35, 35, 55);
        return;
    }

    if (!indicatorsOn) {
        if (lastKey == 0) return;
        lastKey = 0;
        b.allLedsOff();
        return;
    }

    if (presenceIsPresent) {
        if (lastKey == 1) return;
        lastKey = 1;
        b.allLedsOff();
        b.ledOn(6, 0, 70, 45);            // dim teal, one LED
        return;
    }

    // away: a very slow, very faint amber breath on one LED
    unsigned long t = millis() % 5000UL;
    int v = (t < 2500UL) ? (int)(t / 100UL) : (int)((5000UL - t) / 100UL);
    v = 5 + v / 2;                        // 5..17
    long key = 10 + v;
    if (lastKey == key) return;
    lastKey = key;

    b.allLedsOff();
    b.ledOn(1, v, v / 2, 0);
}

// ------------------------------------------------------------------ buttons

static void resetCounters()
{
    events = 0;
    transitions = 0;
    cloudEvents = 0;
    cloudTrans  = 0;
}

static void handleButtons()
{
    if (millis() - lastBtnMs < 15) return;
    lastBtnMs = millis();

    int mask = 0;
    for (uint8_t i = 1; i <= 4; i++) if (b.buttonOn(i)) mask |= (1 << (i - 1));

    int pressed = mask & ~lastMask;
    lastMask = mask;
    if (!pressed) return;

    // A press physically moves the board, so it counts as motion. That is
    // correct — pressing is evidence of presence — and it is why pingPresence
    // is not redundant with the motion detector.
    if (pressed & 0x01) pingPresence();
    if (pressed & 0x02) indicatorsOn = !indicatorsOn;
    if (pressed & 0x04) resetCounters();
    if (pressed & 0x08) {
        // publish now, ignoring the minimum interval
        lastPublishMs = 0;
        requestPublish(presenceIsPresent ? "present" : "away");
    }
}

// -------------------------------------------------------------------- cloud

static void updateCloud()
{
    const char *p = presenceIsPresent ? "present" : "away";
    if (!cloudPresence.equals(p)) cloudPresence = p;

    if (cloudEvents != (int)events) cloudEvents = (int)events;
    if (cloudTrans  != (int)transitions) cloudTrans = (int)transitions;
    if (cloudTimeout != (int)cfgTimeout) cloudTimeout = (int)cfgTimeout;
    int ind = indicatorsOn ? 1 : 0;
    if (cloudInd != ind) cloudInd = ind;
    if (cloudPublished != (int)published) cloudPublished = (int)published;
    int pend = pendingPublish ? 1 : 0;
    if (cloudPending != pend) cloudPending = pend;
}

int fnPing(String)       { pingPresence(); return 1; }
int fnReset(String)      { resetCounters(); return 1; }
int fnPublish(String)    { lastPublishMs = 0; requestPublish(presenceIsPresent ? "present" : "away"); return 1; }

int fnIndicators(String a)
{
    a.trim();
    if (a == "on") indicatorsOn = true;
    else if (a == "off") indicatorsOn = false;
    else indicatorsOn = !indicatorsOn;
    return indicatorsOn ? 1 : 0;
}

// "timeoutSec,threshold,samples" — blank fields keep their current value.
int fnConfig(String arg)
{
    arg.trim();
    if (!arg.length()) return -1;

    long vals[3] = { cfgTimeout, (long)cfgThresh, (long)cfgSamples };

    int idx = 0;
    while (arg.length() && idx < 3) {
        int comma = arg.indexOf(',');
        String tok = (comma < 0) ? arg : arg.substring(0, comma);
        arg = (comma < 0) ? String("") : arg.substring(comma + 1);
        tok.trim();
        if (tok.length()) {
            long v = tok.toInt();
            if (v > 0) vals[idx] = v;
        }
        idx++;
    }

    cfgTimeout = vals[0];
    cfgThresh  = (int)vals[1];
    cfgSamples = (int)vals[2];
    return 1;
}

// -------------------------------------------------------------------- setup

void setup()
{
    b.begin();                     // Internet Button — not begin(1)
    b.setBrightness(RING_BRIGHTNESS);

    Particle.variable("presence",  cloudPresence);
    Particle.variable("last",      cloudLast);      // seconds since last motion
    Particle.variable("events",    cloudEvents);
    Particle.variable("trans",     cloudTrans);
    Particle.variable("published", cloudPublished); // publishes the cloud accepted
    Particle.variable("pending",   cloudPending);
    Particle.variable("move",      cloudMove);
    Particle.variable("timeout",   cloudTimeout);
    Particle.variable("indicators", cloudInd);

    Particle.function("ping",       fnPing);
    Particle.function("reset",      fnReset);
    Particle.function("publish",    fnPublish);
    Particle.function("indicators", fnIndicators);
    Particle.function("config",     fnConfig);

    b.allLedsOff();
    render();
}

// --------------------------------------------------------------------- loop

void loop()
{
    handleButtons();
    checkMotion();
    updatePresence();
    servicePublish();
    render();
    updateCloud();
    delay(5);
}
