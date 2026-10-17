/*
 * simon — the classic "repeat the sequence" memory game on the Particle
 * Internet Button.
 *
 * Four buttons, four colours, four tones. The game plays a growing sequence and
 * you repeat it back. One mistake ends the run.
 *
 *   button 1 (silkscreen "4")  red     C5
 *   button 2 (silkscreen "3")  green   E5
 *   button 3 (silkscreen "2")  blue    G5
 *   button 4 (silkscreen "1")  yellow  C6
 *
 * The silkscreen is reversed — see docs/INIT.md section 8. Pressing a button
 * lights the LED next to it (button i -> LED i+1), so the mapping is
 * self-evident and does not need the labels.
 *
 * BUILD TARGET — do not omit this:
 *   particle compile photon . --target 2.3.1 --saveTo simon.bin
 * The Photon tops out at Device OS 2.3.1, and a binary built for a newer version
 * makes the cloud Safe Mode healer replace the device's firmware over the air
 * (docs/INIT.md sections 6 and 11). Verify with
 *   particle binary inspect simon.bin     -> want "2302"
 *
 * WHY THERE IS NO delay() IN THE PLAYBACK. The obvious way to play a sequence is
 * flash, delay, flash, delay. That blocks loop(), and a 20-step sequence would
 * stall the device for ~10 s — long enough that cloud variable reads time out,
 * which makes the game untestable from outside and would also freeze the
 * accelerometer and button scanning. So playback is a state machine driven by
 * millis(), and the tone is driven by tone()/noTone() rather than the library's
 * playNote(), which blocks and has the duration/uppercase traps noted in
 * docs/INIT.md section 8.
 *
 * WHY THERE IS A "press" FUNCTION. The game is verified by injecting synthetic
 * presses from the cloud: read the exposed `seq`, then press exactly that
 * sequence and confirm the round advances. That tests the real state machine
 * rather than a human's impression of it. Physical buttons still work.
 */

#include "InternetButton.h"

InternetButton b = InternetButton();

// --------------------------------------------------------------------- setup

static const uint8_t  NBTN    = 4;
static const uint8_t  MAX_SEQ = 64;

// Colours are kept dim on purpose: four lit LEDs at full brightness plus a
// buzzer is the kind of load that mattered in docs/INIT.md section 12.
static const uint8_t  LED_R[NBTN] = {150,   0,   0, 150};
static const uint8_t  LED_G[NBTN] = {  0, 150,   0, 130};
static const uint8_t  LED_B[NBTN] = {  0,   0, 150,   0};
static const uint16_t TONE_HZ[NBTN] = {523, 659, 784, 1047};   // C5 E5 G5 C6

// Which LED sits beside each button.
//
// This was WRONG in the first version, which assumed "button i -> LED i+1" from
// a single reading of the library's 3_Buttons_and_LEDs example. The buttons are
// a D-pad, not a row, so a fixed offset cannot work. The correct pairing comes
// from the library's own 4_Good_Combination example (button n -> ledOn(...)):
//
//     button 1 -> LED 12    button 2 -> LED 3
//     button 3 -> LED 6     button 4 -> LED 9
//
// and 7_Internet labels those same buttons up / right / down / left. So the
// buttons sit at the 12 / 3 / 6 / 9 o'clock positions of the ring.
//
// LED 12 is the library's special index for the top: ledOn(12) lights LEDs 1 and
// 11 together (checked in the library source, and ledOff(12) routes through
// ledOn so it clears both). That is why one button lights two LEDs.
static const uint8_t  LED_BTN[NBTN] = {12, 3, 6, 9};

static const uint8_t  RING_BRIGHTNESS = 90;

// playback timing
static int cfgOnMs  = 420;    // how long each step's light+tone lasts
static int cfgGapMs = 170;    // silence between steps

// the brief light+tone acknowledgement when a player presses a button
static const unsigned long PRESS_MS = 220;

// game-over display
static const unsigned long OVER_BLINK_MS = 160;
static const uint8_t       OVER_BLINKS   = 4;
static const unsigned long OVER_SCORE_MS = 2600;

// --------------------------------------------------------------- game state

enum State { ST_IDLE, ST_SHOW, ST_INPUT, ST_OVER, ST_WIN };

static State  state     = ST_IDLE;
static uint8_t seq[MAX_SEQ];
static uint8_t seqLen    = 0;
static uint8_t inputIdx  = 0;
static int     rounds    = 0;   // completed rounds this session
static int     best      = 0;

static bool        soundOn = true;
static bool        toneActive = false;

// playback state machine
static uint8_t       showIdx   = 0;
static uint8_t       showPhase = 0;    // 0 = lit, 1 = gap
static unsigned long phaseAt   = 0;

// transient press acknowledgement
static uint8_t       flashBtn  = 0;    // 0 = none
static unsigned long flashEnd  = 0;

// game-over display
static uint8_t       overBlink = 0;
static bool          overScore = false;
static unsigned long overAt    = 0;

// while set, the ring shows the button-marker LEDs so their positions can be
// confirmed by eye against the physical buttons
static unsigned long mapUntil = 0;

// idle pulse
static unsigned long lastIdleMs = 0;
static int           idleV      = 0;

// button edge detection
static unsigned long lastBtnMs = 0;
static int           lastMask  = 0;

// ------------------------------------------------------------------ cloud UI

static String cloudState = "idle";
static int    cloudLen   = 0;
static int    cloudRound = 0;
static int    cloudBest  = 0;
static int    cloudSpeed = 420;
static int    cloudIdx   = 0;
static String cloudSeq   = "";

// ------------------------------------------------------------------- output

static void ledsOff()
{
    b.allLedsOff();
}

static void showButton(uint8_t i)          // i = 0..3
{
    ledsOff();
    // button i -> the LED physically beside it; see LED_BTN above
    b.ledOn(LED_BTN[i], LED_R[i], LED_G[i], LED_B[i]);
}

static void toneOff()
{
    if (toneActive) { noTone(D0); toneActive = false; }
}

static void toneOn(uint8_t i)
{
    if (!soundOn) return;
    tone(D0, TONE_HZ[i]);
    toneActive = true;
}

// ------------------------------------------------------------------- random

static void addStep()
{
    if (seqLen >= MAX_SEQ) return;
    seq[seqLen++] = (uint8_t)random(NBTN);
}

// -------------------------------------------------------------------- flow

static void enterShow()
{
    state     = ST_SHOW;
    showIdx   = 0;
    showPhase = 0;
    phaseAt   = millis();
    inputIdx  = 0;
    flashBtn  = 0;
    toneOff();
    ledsOff();
    showButton(seq[0]);
    toneOn(seq[0]);
}

static void enterInput()
{
    state    = ST_INPUT;
    inputIdx = 0;
    toneOff();
    ledsOff();
}

static void enterIdle()
{
    state   = ST_IDLE;
    seqLen  = 0;
    toneOff();
    ledsOff();
}

static void enterOver()
{
    state     = ST_OVER;
    overBlink = 0;
    overScore = false;
    overAt    = millis();
    toneOff();
    if (rounds > best) best = rounds;
    ledsOff();
}

static void enterWin()
{
    state = ST_WIN;
    toneOff();
    if (rounds > best) best = rounds;
    ledsOff();
}

static void startGame()
{
    seqLen = 0;
    rounds = 0;
    addStep();
    enterShow();
}

// Register a press of button i (0..3), whether physical or injected.
static void handlePress(uint8_t i)
{
    flashBtn = i + 1;
    flashEnd = millis() + PRESS_MS;
    showButton(i);
    toneOn(i);

    if (state == ST_IDLE) { startGame(); return; }
    if (state == ST_OVER) { enterIdle(); return; }
    if (state == ST_WIN)  { enterIdle(); return; }
    if (state != ST_INPUT) return;      // presses during playback are ignored

    if (i == seq[inputIdx]) {
        inputIdx++;
        if (inputIdx >= seqLen) {
            rounds++;
            addStep();
            enterShow();
        }
    } else {
        enterOver();
    }
}

// -------------------------------------------------------------------- input

static void handleButtons()
{
    if (millis() - lastBtnMs < 8) return;
    lastBtnMs = millis();

    int mask = 0;
    for (uint8_t i = 1; i <= 4; i++) if (b.buttonOn(i)) mask |= (1 << (i - 1));
    int pressed = mask & ~lastMask;
    lastMask = mask;

    for (uint8_t i = 0; i < 4; i++) {
        if (pressed & (1 << i)) handlePress(i);
    }
}

// --------------------------------------------------------- playback machine

static void serviceShow()
{
    if (state != ST_SHOW) return;
    unsigned long now = millis();

    if (showPhase == 0) {
        if (now - phaseAt < (unsigned long)cfgOnMs) return;
        ledsOff();
        toneOff();
        showPhase = 1;
        phaseAt   = now;
        return;
    }

    if (now - phaseAt < (unsigned long)cfgGapMs) return;

    showIdx++;
    if (showIdx >= seqLen) { enterInput(); return; }

    showButton(seq[showIdx]);
    toneOn(seq[showIdx]);
    showPhase = 0;
    phaseAt   = now;
}

// ------------------------------------------------------------------ display

static void render()
{
    unsigned long now = millis();

    // marker map: shows which LED belongs to which button, for eyeball checks
    if ((long)(now - mapUntil) < 0) {
        static long lastMapKey = -2;
        if (lastMapKey != 1) {
            lastMapKey = 1;
            ledsOff();
            for (uint8_t i = 0; i < NBTN; i++)
                b.ledOn(LED_BTN[i], LED_R[i], LED_G[i], LED_B[i]);
        }
        return;
    }

    // press acknowledgement wins over everything else
    if (flashBtn && (long)(now - flashEnd) < 0) { showButton(flashBtn - 1); return; }
    if (flashBtn && (long)(now - flashEnd) >= 0) {
        flashBtn = 0;
        toneOff();
        if (state == ST_SHOW) { ledsOff(); return; }   // playback redraws it
        if (state == ST_INPUT) { ledsOff(); return; }
    }

    if (state == ST_SHOW) return;     // serviceShow owns the display

    if (state == ST_INPUT) { ledsOff(); return; }

    if (state == ST_OVER) {
        if (!overScore) {
            // blink the ring red, all together
            unsigned long elapsed = now - overAt;
            unsigned long step    = elapsed / OVER_BLINK_MS;
            if (step >= OVER_BLINKS * 2UL) {
                overScore = true;
                overAt    = now;
                ledsOff();
                return;
            }
            if (step % 2UL == 0) b.allLedsOn(150, 0, 0);
            else                 ledsOff();
            return;
        }
        // then show the score as a bar for a few seconds
        if (now - overAt > OVER_SCORE_MS) { enterIdle(); return; }
        static long lastBar = -2;
        int n = rounds; if (n > 11) n = 11; if (n < 0) n = 0;
        if (lastBar == n) return;
        lastBar = n;
        ledsOff();
        for (int i = 1; i <= n; i++) b.ledOn(i, 0, 110, 90);
        return;
    }

    if (state == ST_WIN) {
        b.rainbow(20);      // library helper; blocks briefly, acceptable here
        return;
    }

    // idle: the whole ring breathing dim green, "press anything to play".
    // Deliberately not a single LED — button 1's marker is LEDs 1+11, so a lone
    // LED 1 breathing would be ambiguous with it.
    if (now - lastIdleMs < 60) return;
    lastIdleMs = now;
    int v = idleV;
    idleV += 1;
    if (idleV > 40) idleV = 0;
    v = (v <= 20) ? v : 40 - v;                     // triangle 0..20
    b.allLedsOn(0, 4 + v * 2, 2 + v);               // dim green breath
}

// -------------------------------------------------------------------- cloud

static void updateCloud()
{
    const char *s = "idle";
    switch (state) {
        case ST_IDLE:  s = "idle";   break;
        case ST_SHOW:  s = "showing";break;
        case ST_INPUT: s = "input";  break;
        case ST_OVER:  s = "over";   break;
        case ST_WIN:   s = "win";    break;
    }
    if (!cloudState.equals(s)) cloudState = s;

    if (cloudLen   != (int)seqLen)  cloudLen   = (int)seqLen;
    if (cloudRound != rounds)       cloudRound = rounds;
    if (cloudBest  != best)         cloudBest  = best;
    if (cloudSpeed != cfgOnMs)      cloudSpeed = cfgOnMs;
    if (cloudIdx   != (int)inputIdx) cloudIdx  = (int)inputIdx;

    // the sequence as "1,3,2" (1-based) so it can be read and replayed
    if (state == ST_SHOW || state == ST_INPUT) {
        String s2;
        for (uint8_t i = 0; i < seqLen; i++) {
            if (i) s2 += ",";
            s2 += String(seq[i] + 1);
        }
        if (!cloudSeq.equals(s2)) cloudSeq = s2;
    } else if (cloudSeq.length()) {
        cloudSeq = "";
    }
}

int fnStart(String)  { if (state == ST_IDLE || state == ST_OVER || state == ST_WIN) startGame(); return 1; }
int fnReset(String)  { enterIdle(); rounds = 0; return 1; }

// inject a synthetic press: press 1..4, or 0 to clear the short-press flash
int fnPress(String arg)
{
    arg.trim();
    if (arg == "0") { flashBtn = 0; toneOff(); return 0; }
    int n = arg.toInt();
    if (n < 1 || n > 4) return -1;
    handlePress((uint8_t)(n - 1));
    return 1;
}

int fnSound(String a)
{
    a.trim();
    if (a == "on") soundOn = true;
    else if (a == "off") { soundOn = false; toneOff(); }
    else soundOn = !soundOn;
    return soundOn ? 1 : 0;
}

// Show the button-marker LEDs (red top / green right / blue bottom / yellow left)
// for a few seconds so the physical correspondence can be confirmed by eye.
int fnMap(String arg)
{
    arg.trim();
    int secs = arg.length() ? arg.toInt() : 8;
    if (secs < 1)  secs = 8;
    if (secs > 30) secs = 30;
    mapUntil = millis() + (unsigned long)secs * 1000UL;
    return secs;
}

// speed in ms per step; gap is derived. Blank keeps the current value.
int fnSpeed(String arg)
{
    arg.trim();
    if (arg.length()) {
        int v = arg.toInt();
        if (v >= 120 && v <= 1500) { cfgOnMs = v; cfgGapMs = v * 2 / 5; }
    }
    return cfgOnMs;
}

// -------------------------------------------------------------------- setup

void setup()
{
    b.begin();                     // Internet Button — not begin(1)
    b.setBrightness(RING_BRIGHTNESS);

    // seed from clock jitter; the accelerometer is a second source of entropy
    randomSeed(micros() ^ ((unsigned long)b.readX() << 8) ^ (unsigned long)b.readY());

    Particle.variable("state",  cloudState);
    Particle.variable("seq",    cloudSeq);     // "1,3,2" — enough to replay it
    Particle.variable("length", cloudLen);
    Particle.variable("round",  cloudRound);
    Particle.variable("best",   cloudBest);
    Particle.variable("speed",  cloudSpeed);
    Particle.variable("idx",    cloudIdx);

    Particle.function("start", fnStart);
    Particle.function("reset", fnReset);
    Particle.function("press", fnPress);
    Particle.function("sound", fnSound);
    Particle.function("speed", fnSpeed);
    Particle.function("map",   fnMap);

    enterIdle();
    render();
}

// --------------------------------------------------------------------- loop

void loop()
{
    handleButtons();
    serviceShow();
    render();
    updateCloud();
    delay(5);
}
