/*
 * wifi-debug — streams the Photon's Wi-Fi connection attempts over USB serial,
 * so a network that will not join can be diagnosed instead of guessed at.
 *
 * The Photon keeps no log storage: anything Device OS says about a failed join
 * is lost unless a log handler is running at the time. This firmware runs one,
 * and adds its own narration on top:
 *
 *   - the stored credentials (SSID, security, cipher; never the password)
 *   - a scan from the Photon's own radio, so 5 GHz-only networks show up as
 *     missing rather than as a mystery failure
 *   - every state change (off / joining / on network / on cloud / listening),
 *     with how long the previous state lasted and a count of join attempts
 *
 * Watch it with:   particle serial monitor --follow
 *
 * SCAN-ONLY MODE: hold any shield button while pressing RESET, and release once
 * the ring flashes blue. The device then never tries to join; it only scans,
 * every 10 s. That isolates the scan from the system thread's join loop, which
 * may be competing with it for the radio. The ring shows each result:
 *   N LEDs lit = N access points seen (capped at 11)
 *   green = a stored credential's network was among them, amber = it was not
 *   LED 1 red = nothing seen at all
 *
 * BUILD TARGET — this matters, do not omit it:
 *   ../../build.sh wifi-debug
 *
 * The Photon tops out at Device OS 2.3.1; build.sh pins the target and refuses
 * to produce a binary that would trigger the Safe Mode healer (docs/INIT.md §6).
 *
 * LISTENING MODE: `particle serial wifi` talks to the device over the same USB
 * serial port the logs use. So the log handler is torn down while the device is
 * listening, and recreated when it leaves. Close `particle serial monitor`
 * before running `particle serial wifi` — only one program can hold the port.
 */

// The app keeps running while the system thread retries Wi-Fi, which is the
// whole point: with the default single thread, loop() stalls during a join.
SYSTEM_THREAD(ENABLED);

// Connect by hand, after the boot report and scan, so neither is interleaved
// with the first join attempt.
SYSTEM_MODE(SEMI_AUTOMATIC);

// ANTENNA: the Photon has an on-board chip antenna and a u.FL connector, and
// the choice is stored in the device's config, so it survives reflashing. Set
// to external with nothing attached, scans come back near-empty. This pins it
// to the chip antenna. NOTE: that is a persistent change to the device; it
// stays after other firmware is flashed.
STARTUP(WiFi.selectAntenna(ANT_INTERNAL));

#include "InternetButton.h"

InternetButton b = InternetButton();

// Chosen at boot; see SCAN-ONLY MODE above.
static bool g_scanOnly = false;

// Created and destroyed at runtime; see LISTENING MODE above.
static SerialLogHandler* g_log = nullptr;

// ------------------------------------------------------------------ state

enum NetState { ST_OFF, ST_JOINING, ST_NETWORK, ST_CLOUD, ST_LISTENING };

static const char* stateName(NetState s)
{
    switch (s) {
        case ST_OFF:       return "off";
        case ST_JOINING:   return "joining";
        case ST_NETWORK:   return "on network";
        case ST_CLOUD:     return "on cloud";
        case ST_LISTENING: return "listening";
    }
    return "?";
}

static NetState currentState()
{
    if (WiFi.listening())     return ST_LISTENING;
    if (Particle.connected()) return ST_CLOUD;
    if (WiFi.ready())         return ST_NETWORK;
    if (WiFi.connecting())    return ST_JOINING;
    return ST_OFF;
}

static NetState g_state      = ST_OFF;
static uint32_t g_enteredAt  = 0;   // millis() when g_state began
static uint32_t g_lastBeat   = 0;
static int      g_attempts   = 0;   // network "connecting" events
static int      g_failures   = 0;   // "disconnected" while still joining

// Stored credentials, read once at boot, and how often each has turned up in a
// scan. One scan can miss a beacon; a tally over many cannot be a fluke.
static WiFiAccessPoint g_creds[5];
static int      g_nCreds     = 0;
static int      g_seenIn[5]  = {0, 0, 0, 0, 0};
static int      g_scans      = 0;
static uint32_t g_lastScan   = 0;

// Rescan this often while off the cloud. Scan-only mode has nothing else to do.
static const uint32_t SCAN_EVERY_MS      = 30000;
static const uint32_t SCAN_ONLY_EVERY_MS = 10000;

// Readable from the cloud once it does connect, so the history of how it got
// there survives without a USB cable attached.
static String   g_summary    = "";

// ------------------------------------------------------------------ helpers

static const char* securityName(int s)
{
    switch (s) {
        case WLAN_SEC_UNSEC:           return "open";
        case WLAN_SEC_WEP:             return "WEP";
        case WLAN_SEC_WPA:             return "WPA";
        case WLAN_SEC_WPA2:            return "WPA2";
        case WLAN_SEC_WPA_ENTERPRISE:  return "WPA-Enterprise";
        case WLAN_SEC_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    }
    return "unset";
}

static const char* cipherName(int c)
{
    switch (c) {
        case WLAN_CIPHER_AES:      return "AES";
        case WLAN_CIPHER_TKIP:     return "TKIP";
        case WLAN_CIPHER_AES_TKIP: return "AES+TKIP";
    }
    return "unset";
}

static void reportCredentials(WiFiAccessPoint* creds, int n)
{
    Log.info("stored credentials: %d (the Photon tries each in turn)", n);
    for (int i = 0; i < n; i++) {
        Log.info("  [%d] \"%s\"  %s / %s", i, creds[i].ssid,
                 securityName(creds[i].security), cipherName(creds[i].cipher));
    }
    if (n == 0) Log.warn("  none — the device will go to listening mode");
}

// The Photon's radio is 2.4 GHz only, so this is what the device can actually
// see, which can differ sharply from a laptop's scan of the same room.
//
// At boot WiFi.on() returns before the radio is up, and a scan issued then
// reports zero networks rather than an error, hence `tries`.
// Returns the number of access points seen; sets `credSeen` if any stored
// credential's network was among them.
static int reportScan(int tries, bool* credSeen = nullptr)
{
    static WiFiAccessPoint aps[30];

    int found = 0;
    for (int t = 1; t <= tries; t++) {
        found = WiFi.scan(aps, 30);
        if (found > 0) break;
        Log.trace("scan try %d: %d", t, found);
        if (t < tries) delay(1000);
    }
    g_scans++;
    g_lastScan = millis();
    Log.info("scan #%d (%s): %d access points visible to the Photon",
             g_scans, stateName(g_state), found);

    for (int i = 0; i < found; i++) {
        Log.info("  ch %2d  %4d dBm  %-15s  \"%s\"", aps[i].channel, aps[i].rssi,
                 securityName(aps[i].security), aps[i].ssid);
    }

    for (int c = 0; c < g_nCreds; c++) {
        int seen = 0, best = -127;
        for (int i = 0; i < found; i++) {
            if (strcmp(aps[i].ssid, g_creds[c].ssid) == 0) {
                seen++;
                if (aps[i].rssi > best) best = aps[i].rssi;
            }
        }
        if (seen) g_seenIn[c]++;
        if (seen && credSeen) *credSeen = true;
        if (seen) {
            Log.info("credential \"%s\": seen on %d AP(s), best %d dBm  [in %d/%d scans]",
                     g_creds[c].ssid, seen, best, g_seenIn[c], g_scans);
        } else {
            Log.warn("credential \"%s\": NOT seen  [in %d/%d scans]",
                     g_creds[c].ssid, g_seenIn[c], g_scans);
        }
    }
    return found;
}

static void reportNetwork()
{
    byte bssid[6];
    WiFi.BSSID(bssid);
    IPAddress ip = WiFi.localIP();
    int rssi = WiFi.RSSI();
    Log.info("joined \"%s\"  bssid %02x:%02x:%02x:%02x:%02x:%02x  %d dBm  ip %d.%d.%d.%d",
             WiFi.SSID(), bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
             rssi, ip[0], ip[1], ip[2], ip[3]);
}

static void updateSummary()
{
    g_summary = String::format("state=%s ssid=%s attempts=%d failures=%d",
                               stateName(g_state), WiFi.ready() ? WiFi.SSID() : "",
                               g_attempts, g_failures);
}

// Device OS's own view of each step. Unlike WiFi.connecting(), which stays true
// across every retry, these fire once per transition, so they show the
// individual join attempts.
static const char* networkEventName(int param)
{
    switch (param) {
        case network_status_powering_on:  return "powering on";
        case network_status_on:           return "radio on";
        case network_status_connecting:   return "connecting";
        case network_status_connected:    return "connected";
        case network_status_disconnecting: return "disconnecting";
        case network_status_disconnected: return "disconnected";
        case network_status_powering_off: return "powering off";
        case network_status_off:          return "radio off";
    }
    return "?";
}

static void onSystemEvent(system_event_t event, int param)
{
    if (event == network_status) {
        if (param == network_status_connecting) g_attempts++;
        if (param == network_status_disconnected && g_state == ST_JOINING) g_failures++;
        Log.info("event: network %s (%d)", networkEventName(param), param);
    } else if (event == cloud_status) {
        Log.info("event: cloud status %d", param);
    }
    updateSummary();
}

// The ring as a scan meter, so scan-only mode can be read without a cable.
static void showScan(int found, bool credSeen)
{
    b.allLedsOff();
    if (found <= 0) { b.ledOn(1, 60, 0, 0); return; }
    for (int i = 1; i <= found && i <= 11; i++) {
        if (credSeen) b.ledOn(i, 0, 60, 0);
        else          b.ledOn(i, 60, 30, 0);
    }
}

// ------------------------------------------------------------------- setup

void setup()
{
    g_log = new SerialLogHandler(LOG_LEVEL_ALL);

    // No argument: this is the Internet Button shield (see shield-check).
    b.begin();
    for (uint8_t i = 1; i <= 4; i++) {
        if (b.buttonOn(i)) g_scanOnly = true;
    }
    if (g_scanOnly) {
        b.allLedsOn(0, 0, 60);   // acknowledge: the button can be released
        delay(1000);
        b.allLedsOff();
    }

    // Give `particle serial monitor` a chance to attach, so the boot report is
    // not printed to nobody. Continues regardless after 10 s.
    // The host drops whatever arrives while it is still opening the port, so
    // pause briefly once connected.
    if (waitFor(Serial.isConnected, 10000)) delay(1500);

    Log.info("wifi-debug: Device OS %s, antenna pinned to internal", System.version().c_str());
    if (g_scanOnly) Log.info("SCAN-ONLY MODE: not joining any network; scanning every %lu s",
                             (unsigned long)(SCAN_ONLY_EVERY_MS / 1000));

    Particle.variable("wifi", g_summary);
    System.on(network_status | cloud_status, onSystemEvent);

    WiFi.on();

    g_nCreds = WiFi.getCredentials(g_creds, 5);
    reportCredentials(g_creds, g_nCreds);
    bool credSeen = false;
    int found = reportScan(8, &credSeen);
    g_enteredAt = millis();

    if (g_scanOnly) {
        showScan(found, credSeen);
        return;
    }

    Log.info("connecting...");
    Particle.connect();
}

// -------------------------------------------------------------------- loop

void loop()
{
    NetState now = currentState();

    // Keep the USB port clear while `particle serial wifi` is using it.
    if (now == ST_LISTENING && g_log) {
        delete g_log;
        g_log = nullptr;
    } else if (now != ST_LISTENING && !g_log) {
        g_log = new SerialLogHandler(LOG_LEVEL_ALL);
    }

    if (now != g_state) {
        uint32_t held = millis() - g_enteredAt;

        Log.info(">> %s -> %s  (after %lu ms)  attempts=%d failures=%d",
                 stateName(g_state), stateName(now), (unsigned long)held,
                 g_attempts, g_failures);

        if (now == ST_NETWORK || (now == ST_CLOUD && g_state != ST_NETWORK)) reportNetwork();

        g_state     = now;
        g_enteredAt = millis();
        updateSummary();
    }

    // Keep scanning while it is not getting anywhere. Skipped on the cloud,
    // where there is nothing left to diagnose, and while listening, where the
    // radio is the setup interface.
    if (g_scanOnly) {
        if (millis() - g_lastScan >= SCAN_ONLY_EVERY_MS) {
            bool credSeen = false;
            int found = reportScan(1, &credSeen);
            showScan(found, credSeen);
        }
    } else if (g_state != ST_CLOUD && g_state != ST_LISTENING &&
               millis() - g_lastScan >= SCAN_EVERY_MS) {
        reportScan(1);
    }

    // A heartbeat, so a long silent join is visibly still in progress rather
    // than looking like a hung device.
    if (millis() - g_lastBeat >= 5000) {
        g_lastBeat = millis();
        Log.info(".. %s for %lu s", stateName(g_state),
                 (unsigned long)((millis() - g_enteredAt) / 1000));
    }

    delay(100);
}
