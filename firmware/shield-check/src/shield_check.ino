/*
 * shield-check — exercises every peripheral on the Particle Internet Button
 * shield, and reports the result over the Particle cloud.
 *
 * Why it reports over the cloud: the LEDs and the buzzer can only be confirmed
 * by eye and ear, but the buttons and the accelerometer are electrical, so they
 * are exposed as Particle variables and can be read back and checked
 * programmatically. That way the check is not purely "does it look right".
 *
 * BUILD TARGET — this matters, do not omit it:
 *   particle compile photon . --target 2.3.1 --saveTo shield-check.bin
 *
 * The Photon tops out at Device OS 2.3.1. Compiling without an explicit target
 * can produce a binary declaring a dependency on a newer Device OS, and the
 * cloud Safe Mode healer will then silently replace the device's firmware with
 * it (see docs/INIT.md sections 6 and 11). The symptoms look like a spontaneous
 * upgrade with no USB traffic at all.
 *
 * NOTE: flashing this replaces Tinker. That is expected — Tinker cannot drive
 * WS2812B LEDs, so the shield cannot be tested while Tinker is running.
 */

#include "InternetButton.h"

InternetButton b = InternetButton();

// ---------------------------------------------------------------- cloud state

// bit 0..3 => button 1..4 currently held down
int    g_buttons = 0;

// "x,y,z" from the ADXL362, 8-bit signed per axis
String g_accel   = "0,0,0";

// Latched: bit 0..3 => button i has been pressed at least once since the last
// reset. Polling "buttons" alone samples at roughly 1 Hz, so a quick tap falls
// between reads and looks like a dead button. Latching makes the check robust to
// that: a single read afterwards reports every button that was ever pressed.
int    g_seen     = 0;
int    g_counts[4] = {0, 0, 0, 0};
String g_countStr  = "0,0,0,0";

// previous raw states, for edge detection
static int g_prevRaw = 0;

// --------------------------------------------------------------- led render

// 0 = reflect the buttons, 1 = all LEDs one colour, 2 = a single LED, 3 = dark
static int     g_mode   = 0;
static uint8_t g_r = 0, g_g = 0, g_b = 0;
static int     g_single = 1;

// The library's own example pairs button 2 with LED 3, so the shield's buttons
// sit one position behind the LED that faces them: button i -> LED i+1.
// (docs/INIT.md section 8 records the separate silkscreen bug, where the
// buttons are printed 4-3-2-1 but are really 1-2-3-4.)
static uint8_t ledForButton(uint8_t i) { return i + 1; }

static void render()
{
    if (g_mode == 3) { b.allLedsOff(); return; }

    if (g_mode == 1) { b.allLedsOn(g_r, g_g, g_b); return; }

    if (g_mode == 2) {
        b.allLedsOff();
        b.ledOn(g_single, 80, 80, 80);
        return;
    }

    // button mode — each held button lights its paired LED
    b.allLedsOff();
    for (uint8_t i = 1; i <= 4; i++) {
        if (g_buttons & (1 << (i - 1))) b.ledOn(ledForButton(i), 0, 60, 60);
    }
}

// ------------------------------------------------------------------- helpers

// Light LEDs 1..11 one at a time, then all together. This is the part that
// needs a human: LED count, order, and colour.
static void sweep()
{
    int savedMode = g_mode;
    g_mode = 3;
    for (uint8_t i = 1; i <= 11; i++) {
        b.allLedsOff();
        b.ledOn(i, 70, 70, 70);
        delay(220);
    }
    b.allLedsOff();
    b.allLedsOn(70, 70, 70);
    delay(400);
    b.allLedsOff();
    g_mode = savedMode;
}

// ------------------------------------------------------------------ handlers

int ledFunc(String arg)
{
    arg.trim();
    arg.toLowerCase();

    if (arg == "off")   { g_mode = 3; b.allLedsOff();                    return 0; }
    if (arg == "white") { g_mode = 1; g_r = 70; g_g = 70; g_b = 70;      return 70; }
    if (arg == "red")   { g_mode = 1; g_r = 90; g_g = 0;  g_b = 0;       return 90; }
    if (arg == "green") { g_mode = 1; g_r = 0;  g_g = 90; g_b = 0;       return 90; }
    if (arg == "blue")  { g_mode = 1; g_r = 0;  g_g = 0;  g_b = 90;      return 90; }
    if (arg == "buttons" || arg == "auto") { g_mode = 0;                 return 0; }

    int n = arg.toInt();
    if (n >= 1 && n <= 11) { g_mode = 2; g_single = n; return n; }
    return -1;
}

int sweepFunc(String)      { sweep(); return 1; }

int beepFunc(String arg)
{
    arg.trim();
    if (arg.length() == 0) arg = "C5";
    b.playNote(arg, 400);
    return 1;
}

// Clear the latched view so a fresh press test can start from a known state.
int resetFunc(String)
{
    g_seen   = 0;
    g_counts[0] = g_counts[1] = g_counts[2] = g_counts[3] = 0;
    g_countStr = "0,0,0,0";
    return 1;
}

// --------------------------------------------------------------------- setup

void setup()
{
    // No argument: this is the Internet Button shield. begin(1) is only for the
    // original SparkButton, which has no buzzer and different button pins.
    b.begin();
    b.setBrightness(128);

    Particle.variable("buttons", g_buttons);   // currently held, bit 0..3
    Particle.variable("seen",    g_seen);      // latched since last reset
    Particle.variable("counts",  g_countStr);  // "c1,c2,c3,c4", latched
    Particle.variable("accel",   g_accel);
    Particle.function("led",     ledFunc);
    Particle.function("sweep",   sweepFunc);
    Particle.function("beep",    beepFunc);
    Particle.function("reset",   resetFunc);

    sweep();   // visual self-test on every boot
}

// ---------------------------------------------------------------------- loop

void loop()
{
    int mask = 0;
    for (uint8_t i = 1; i <= 4; i++) {
        if (b.buttonOn(i)) mask |= (1 << (i - 1));
    }
    g_buttons = mask;

    // Latch every up->down transition, so a press too brief to be caught by a
    // ~1 Hz cloud poll is still recorded and cannot masquerade as a dead button.
    for (uint8_t i = 0; i < 4; i++) {
        int now = (mask >> i) & 1;
        int was = (g_prevRaw >> i) & 1;
        if (now && !was) {
            g_counts[i]++;
            g_seen |= (1 << i);
            g_countStr = String(g_counts[0]) + "," + String(g_counts[1]) + "," +
                         String(g_counts[2]) + "," + String(g_counts[3]);
        }
    }
    g_prevRaw = mask;

    g_accel = String(b.readX()) + "," + String(b.readY()) + "," + String(b.readZ());

    render();
    delay(20);
}
