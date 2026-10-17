/*
 * interval-timer — a pomodoro-style interval timer on the Particle Internet Button.
 *
 * The 11 LEDs are a physical progress bar: all lit at the start of a phase, one
 * going dark at a time as it runs out. Amber = work, green = break, violet = long
 * break, blue = paused.
 *
 * BUILD TARGET — do not omit this:
 *   particle compile photon . --target 2.3.1 --saveTo interval-timer.bin
 * The Photon tops out at Device OS 2.3.1, and a binary built for a newer version
 * makes the cloud Safe Mode healer replace the device's firmware over the air
 * (docs/INIT.md sections 6 and 11). Check with
 *   particle binary inspect interval-timer.bin     -> want "2302"
 *
 * BUTTONS. The shield's silkscreen is wrong (docs/INIT.md section 8): the labels
 * are printed 4-3-2-1 but the truth is 1-2-3-4. Mapping used here:
 *
 *   button 1  (silkscreened "4")  start / pause / resume
 *   button 2  (silkscreened "3")  skip to the next phase
 *   button 3  (silkscreened "2")  reset to idle
 *   button 4  (silkscreened "1")  sound on / off
 *
 * Pressing any button flashes its paired LED (button i -> LED i+1) for 400 ms, so
 * you can identify them without trusting the print. That flash is suppressed while
 * the timer is running, so it never hides the progress bar mid-session.
 *
 * POWER. Colours are deliberately dim and the ring brightness is capped. All 11
 * WS2812B at full white is ~660 mA, which is the one load that came close to the
 * brownout threshold in docs/INIT.md section 12. Do not remove the cap casually.
 */

#include "InternetButton.h"

InternetButton b = InternetButton();

// ------------------------------------------------------------------ settings

static long cfgWork    = 25L * 60;    // work phase, seconds
static long cfgBreak   =  5L * 60;    // short break
static long cfgLongBrk = 15L * 60;    // long break
static int  cfgPerLong = 4;           // a long break after every N work intervals

static bool soundOn     = true;
static bool autoPauseOn = true;       // pause when the button is picked up

static const uint8_t RING_BRIGHTNESS = 70;

// movement detection, in 8-bit ADXL362 LSB summed across the three axes
// (at rest the per-sample change is ~0-4; picking the board up exceeds 20)
static const int  MOVE_THRESH    = 15;
static const int  MOVE_SAMPLES   = 3;      // consecutive moving samples to act on
static const unsigned long ACCEL_MS    = 100;
static const unsigned long SUPPRESS_MS = 800;   // ignore movement after a press

// Paused indication. A steady bar is too easy to miss, and a bar that goes dark
// reads as a dead device, so the bar pulses between full and dim and never
// switches off. An initial fast burst marks the moment of pausing; after that it
// settles into a slow pulse.
static const unsigned long PAUSE_BURST_MS  = 1100;
static const unsigned long PAUSE_BURST_STEP = 110;   // burst half-cycle
static const unsigned long PAUSE_PULSE_MS   = 350;   // steady half-cycle
static const uint8_t       PAUSE_DIM        = 55;    // never 0 — see above

// --------------------------------------------------------------------- state

enum Phase { PH_IDLE, PH_WORK, PH_BREAK };

static Phase phase       = PH_IDLE;
static bool  running     = false;
static bool  isLongBreak = false;
static bool  autoPaused  = false;

static unsigned long endMs    = 0;    // millis() when the current phase ends
static long          totalSec = 0;
static long          remainSec = 0;
static int           workDone = 0;    // completed work intervals

// ------------------------------------------------------------------ cloud UI

static String cloudState  = "idle";
static int    cloudRemain = 0;
static int    cloudCycle  = 0;
static int    cloudRun    = 0;
static int    cloudMove   = 0;    // last movement delta (diagnostic)
static int    cloudSeen   = 0;    // latched presses, bit 0..3 (identify buttons)

// Why we are paused, so the cloud can distinguish the two cases: "" while
// running or idle, "manual" for a button press, "auto" for movement.
static String cloudPaused = "";

static int           pauseReason   = 0;    // 0 none, 1 manual, 2 auto
static unsigned long announceStart = 0;    // start of the pause attention burst

// --------------------------------------------------------------- ui internals

static int           lastMask      = 0;
static unsigned long lastBtnMs     = 0;
static unsigned long suppressMove  = 0;
static int           pressFlashBtn = 0;
static unsigned long pressFlashEnd = 0;
static long          lastRenderKey = -2;
static unsigned long lastPulseMs   = 0;

static unsigned long lastAccelMs = 0;
static int  ax = 0, ay = 0, az = 0;
static bool haveAccel = false;
static int  moveRun   = 0;

// ------------------------------------------------------------------- chimes
//
// playNote(note, d) plays for 1000*240/bpm/d ms, and bpm defaults to 250 — so the
// argument is a *subdivision*, not a duration. d=4 is 240 ms, d=8 is 120 ms.
// Passing a large number here (e.g. 400) yields a ~2 ms click that is inaudible.
// playNote blocks for the note's length, so keep chimes short.

static void chimeStart()   { if (soundOn) b.playNote("C5", 4); }
static void chimeToBreak() { if (soundOn) { b.playNote("E5", 4); b.playNote("G5", 4); } }
static void chimeToLong()  { if (soundOn) { b.playNote("G5", 4); b.playNote("C6", 4); b.playNote("E6", 4); } }
static void chimeToWork()  { if (soundOn) { b.playNote("G5", 4); b.playNote("E5", 4); } }
static void chimeReset()   { if (soundOn) { b.playNote("E5", 6); b.playNote("C5", 6); } }
static void chimeMove()    { if (soundOn) b.playNote("E5", 8); }

// ------------------------------------------------------------------- phases

static void startPhase(Phase p, bool longBreak)
{
    phase       = p;
    isLongBreak = longBreak;
    totalSec    = (p == PH_WORK) ? cfgWork : (longBreak ? cfgLongBrk : cfgBreak);
    endMs       = millis() + (unsigned long)totalSec * 1000UL;
    remainSec   = totalSec;
    running     = true;
    autoPaused  = false;
    lastRenderKey = -2;          // force a redraw
}

static void beginWork()
{
    chimeStart();
    startPhase(PH_WORK, false);
}

static void completePhase()
{
    if (phase == PH_WORK) {
        workDone++;
        bool longBreak = (cfgPerLong > 0) && (workDone % cfgPerLong == 0);
        if (longBreak) chimeToLong(); else chimeToBreak();
        startPhase(PH_BREAK, longBreak);
    } else {
        chimeToWork();
        startPhase(PH_WORK, false);
    }
}

static void gotoIdle(bool quiet)
{
    phase       = PH_IDLE;
    running     = false;
    isLongBreak = false;
    autoPaused  = false;
    pauseReason = 0;
    remainSec   = 0;
    totalSec    = 0;
    lastRenderKey = -2;
    if (!quiet) chimeReset();
}

static void togglePause()
{
    if (phase == PH_IDLE) { beginWork(); return; }
    if (running) {
        unsigned long left = endMs - millis();
        remainSec = (long)((left + 999UL) / 1000UL);
        running   = false;
        pauseReason   = 1;                 // manual
        announceStart = millis();          // kick off the attention burst
    } else {
        endMs     = millis() + (unsigned long)remainSec * 1000UL;
        running   = true;
        autoPaused = false;
        pauseReason = 0;
    }
    lastRenderKey = -2;
}

// ------------------------------------------------------------------ handlers

static void onButton1() { togglePause(); }
static void onButton2() { if (phase != PH_IDLE) completePhase(); }
static void onButton3() { workDone = 0; gotoIdle(false); }
static void onButton4() { soundOn = !soundOn; if (soundOn) b.playNote("G5", 6); }

static void handleButtons()
{
    if (millis() - lastBtnMs < 15) return;
    lastBtnMs = millis();

    int mask = 0;
    for (uint8_t i = 1; i <= 4; i++) if (b.buttonOn(i)) mask |= (1 << (i - 1));

    int pressed = mask & ~lastMask;      // up -> down edges only
    lastMask = mask;
    if (!pressed) return;

    suppressMove = millis() + SUPPRESS_MS;   // a press moves the board
    cloudSeen |= pressed;

    // Show which button was pressed, but never hide a running progress bar.
    if (phase == PH_IDLE || !running) {
        for (uint8_t i = 0; i < 4; i++) {
            if (pressed & (1 << i)) { pressFlashBtn = i + 1; pressFlashEnd = millis() + 400; }
        }
    }

    if (pressed & 0x01) onButton1();
    if (pressed & 0x02) onButton2();
    if (pressed & 0x04) onButton3();
    if (pressed & 0x08) onButton4();

    lastRenderKey = -2;
}

// -------------------------------------------------------------------- timer

static void updateTimer()
{
    if (!running) return;
    unsigned long left = endMs - millis();
    if (left == 0 || left > 100000000UL) {   // elapsed (the large value guards wrap)
        remainSec = 0;
        completePhase();
        return;
    }
    remainSec = (long)((left + 999UL) / 1000UL);
}

// --------------------------------------------------------------- auto-pause

static void checkMovement()
{
    if (millis() - lastAccelMs < ACCEL_MS) return;
    lastAccelMs = millis();

    int x = b.readX(), y = b.readY(), z = b.readZ();
    if (!haveAccel) { ax = x; ay = y; az = z; haveAccel = true; return; }

    int d = abs(x - ax) + abs(y - ay) + abs(z - az);
    ax = x; ay = y; az = z;
    cloudMove = d;

    if (!autoPauseOn || !running || millis() < suppressMove) { moveRun = 0; return; }

    if (d > MOVE_THRESH) moveRun++; else moveRun = 0;

    if (moveRun >= MOVE_SAMPLES) {
        unsigned long left = endMs - millis();
        remainSec  = (long)((left + 999UL) / 1000UL);
        running    = false;
        autoPaused = true;
        pauseReason   = 2;             // auto, by movement
        announceStart = millis();
        moveRun    = 0;
        lastRenderKey = -2;
        chimeMove();
    }
}

// ------------------------------------------------------------------ display

static void render(bool force)
{
    // transient button-identification flash
    if (pressFlashEnd && millis() < pressFlashEnd) {
        long key = -100 - pressFlashBtn;
        if (!force && key == lastRenderKey) return;
        lastRenderKey = key;
        b.allLedsOff();
        b.ledOn(pressFlashBtn + 1, 120, 120, 120);
        return;
    }

    if (phase == PH_IDLE) {
        if (!force && millis() - lastPulseMs < 100) return;
        lastPulseMs = millis();
        lastRenderKey = -999;
        unsigned long t = millis() % 2400;
        int v = (t < 1200) ? (int)(t / 20) : (int)((2400 - t) / 20);   // 0..60
        b.allLedsOff();
        b.ledOn(1, 0, 0, 8 + v);                                       // slow blue pulse
        return;
    }

    int filled = 0;
    if (totalSec > 0) {
        filled = (int)((11L * remainSec + totalSec - 1) / totalSec);   // ceil
    }
    if (filled > 11) filled = 11;
    if (filled < 0)  filled = 0;

    // The phase colour is kept while paused, so the bar still answers "which
    // phase am I in" — only the *dynamics* change, which is the whole cue.
    uint8_t r, g, bl;
    if (phase == PH_WORK)      { r = 255; g = 110; bl =   0; }   // work: amber
    else if (isLongBreak)      { r = 120; g =   0; bl = 255; }   // long break: violet
    else                       { r =   0; g = 220; bl =  80; }   // short break: green

    // ---- paused: pulse the bar
    if (!running) {
        unsigned long t = millis();
        unsigned long since = t - announceStart;

        bool lit;
        if (since < PAUSE_BURST_MS) {
            lit = ((since / PAUSE_BURST_STEP) % 2) == 0;   // fast burst on pausing
        } else {
            lit = ((t / PAUSE_PULSE_MS) % 2) == 0;         // then a slow pulse
        }

        int show = (filled < 1) ? 1 : filled;   // never a completely dark ring

        long key = 20000 + (long)show * 2 + (lit ? 1 : 0);
        if (!force && key == lastRenderKey) return;
        lastRenderKey = key;

        // dim rather than off, so a paused timer never reads as a dead device
        uint8_t s = lit ? 255 : PAUSE_DIM;
        uint8_t pr = (uint8_t)((uint16_t)r  * s / 255);
        uint8_t pg = (uint8_t)((uint16_t)g  * s / 255);
        uint8_t pb = (uint8_t)((uint16_t)bl * s / 255);

        for (uint8_t i = 1; i <= 11; i++) {
            if ((int)i <= show) b.ledOn(i, pr, pg, pb);
            else                b.ledOff(i);
        }
        return;
    }

    // ---- running: a steady bar
    long key = (long)phase * 100 + filled * 2;
    if (!force && key == lastRenderKey) return;
    lastRenderKey = key;

    for (uint8_t i = 1; i <= 11; i++) {
        if ((int)i <= filled) b.ledOn(i, r, g, bl);
        else                  b.ledOff(i);
    }
}

// -------------------------------------------------------------------- cloud

static void updateCloud()
{
    const char *s = "idle";
    if (phase == PH_WORK)       s = "work";
    else if (phase == PH_BREAK) s = isLongBreak ? "longbreak" : "break";
    if (!cloudState.equals(s)) cloudState = s;

    int rm = running ? (int)remainSec : (remainSec > 0 ? (int)remainSec : 0);
    if (cloudRemain != rm) cloudRemain = rm;
    if (cloudCycle != workDone) cloudCycle = workDone;

    int rn = running ? 1 : 0;
    if (cloudRun != rn) cloudRun = rn;

    const char *p = (pauseReason == 2) ? "auto"
                  : (pauseReason == 1) ? "manual"
                  : "";
    if (!cloudPaused.equals(p)) cloudPaused = p;
}

int fnStart(String)  { togglePause(); return 1; }
int fnPause(String)  { if (running) togglePause(); return 1; }
int fnReset(String)  { workDone = 0; gotoIdle(false); return 1; }
int fnSkip(String)   { if (phase != PH_IDLE) completePhase(); return 1; }
int fnSound(String a)     { a.trim(); if (a == "on") soundOn = true; else if (a == "off") soundOn = false; else soundOn = !soundOn; return soundOn ? 1 : 0; }
int fnAutopause(String a) { a.trim(); if (a == "on") autoPauseOn = true; else if (a == "off") autoPauseOn = false; else autoPauseOn = !autoPauseOn; return autoPauseOn ? 1 : 0; }
int fnClearSeen(String)   { cloudSeen = 0; return 1; }

// "workSec,breakSec,longSec,perLong" — blank fields keep their current value,
// so a partial update like "60,15" only changes work and break. Applies to the
// next phase; use reset+start to restart with the new values.
int fnConfig(String arg)
{
    arg.trim();
    if (!arg.length()) return -1;

    long vals[4] = { cfgWork, cfgBreak, cfgLongBrk, (long)cfgPerLong };

    int idx = 0;
    while (arg.length() && idx < 4) {
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

    cfgWork = vals[0];
    cfgBreak = vals[1];
    cfgLongBrk = vals[2];
    cfgPerLong = (int)vals[3];
    return 1;
}

// --------------------------------------------------------------------- setup

void setup()
{
    b.begin();                    // Internet Button — not begin(1), which is SparkButton-only
    b.setBrightness(RING_BRIGHTNESS);

    Particle.variable("state",   cloudState);
    Particle.variable("remain",  cloudRemain);
    Particle.variable("cycle",   cloudCycle);
    Particle.variable("running", cloudRun);
    Particle.variable("paused",  cloudPaused);   // "" | "manual" | "auto"
    Particle.variable("move",    cloudMove);
    Particle.variable("seen",    cloudSeen);

    Particle.function("start",     fnStart);
    Particle.function("pause",     fnPause);
    Particle.function("reset",     fnReset);
    Particle.function("skip",      fnSkip);
    Particle.function("sound",     fnSound);
    Particle.function("autopause", fnAutopause);
    Particle.function("config",    fnConfig);
    Particle.function("clearseen", fnClearSeen);

    b.allLedsOff();
    render(true);
}

// ---------------------------------------------------------------------- loop

void loop()
{
    handleButtons();
    updateTimer();
    checkMovement();
    render(false);
    updateCloud();
    delay(5);
}
