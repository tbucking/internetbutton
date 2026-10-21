# wifi-debug

Streams the Photon's Wi-Fi connection attempts over USB serial, so a network that won't join can be
diagnosed instead of guessed at.

The Photon keeps no logs. Anything Device OS reports about a failed join is lost unless a log handler is
running at the time. This firmware runs one at `LOG_LEVEL_ALL` and adds its own report:

- **Stored credentials:** SSID, security and cipher for each. The password is never shown.
- **A scan from the Photon's own radio:** it's 2.4 GHz only, so this lists what the device can actually
  see. Each stored SSID is marked as seen or **NOT seen**.
- **Every state change:** `off`, `joining`, `on network`, `on cloud`, `listening`, with how long the
  previous state lasted and running counts of join attempts and failures.
- **The access point it joined:** SSID, BSSID, RSSI and IP address.
- **A heartbeat every 5 s:** a long, silent join visibly stays in progress.

The same counters are exposed as the cloud variable `wifi`, so the history survives after the cable is
unplugged: `particle get <device> wifi`.

## Build and flash

```sh
./build.sh wifi-debug --flash
```

Flash it **while the device is still on a working network**. `--flash` goes over the cloud, which won't
work once the device can't get online. This replaces whatever firmware is running.

## Diagnosing a network

```sh
# 1. Hold SETUP ~3 s until the LED blinks dark blue (listening mode), then:
particle serial wifi --file ~/credentials.json     # see docs/INIT.md §7

# 2. Once the CLI exits, watch the attempt:
particle serial monitor --follow
```

Only one program can hold the USB port, so **close the monitor before running `particle serial wifi`.**
The firmware also turns its own logging off while the device is in listening mode, so log lines don't
get mixed into the CLI's exchange with the device.

To capture the boot report (credentials and scan), start the monitor and then press RESET. The firmware
waits up to 10 s for a serial connection before printing.

## Scan-only mode

Hold **any shield button** while tapping **RESET**, and let go once the ring flashes blue. The device
then never tries to join a network. It only scans, every 10 s. This takes the system's join attempts
out of the picture, since they may compete with the scan for the radio.

The LED ring shows each result, so you don't need a cable:

| Ring | Meaning |
| --- | --- |
| N LEDs lit (up to 11) | N access points seen |
| Green | A saved network was among them |
| Amber | None of the saved networks was among them |
| LED 1 red | Nothing seen at all |

Tap RESET without holding a button to go back to normal mode.

## Reading the output

| What you see | What it means |
| --- | --- |
| `credential "X": NOT seen` | The radio can't see X: out of range, hidden, or 5 GHz only. No password will help |
| Repeated `joining -> off`, `failures` climbing | Seen but rejected: wrong password, wrong security type or, on Enterprise, wrong EAP method, username or CA |
| `on network`, then never `on cloud` | Joined Wi-Fi, but the Particle cloud is unreachable: a captive portal or blocked outbound traffic |
| `joined "Y"` where you expected X | The Photon stores up to 5 networks and tries each in turn, and Y answered first. Hold SETUP for 10 s to clear them |

The `event: network …` lines are Device OS's own events. Each `connecting` marks a join attempt; a
`disconnected` about 65 s later marks a full pass through the saved networks that failed. Device OS 2.3.1
on the Photon printed no Wi-Fi driver messages in testing, even at `LOG_LEVEL_ALL`. So a failed join
gives no specific reason; the scan results are the more useful evidence.

## Status

Run on the device. The boot report, scans, state changes, Device OS network events, scan-only mode and
the LED meter all work as described.

Still unverified: how quiet the USB port stays while `particle serial wifi` runs, and how much detail
Device OS 2.3.1 logs for an Enterprise failure. In practice it logs network events but no driver-level
messages, even at `LOG_LEVEL_ALL`.

First use (2026-10-21): powered from a laptop, the Photon saw 0–2 networks per scan, while the laptop
saw about ten. Whether that was the laptop's USB power or the Photon's antenna is still open; the next
test is the same scan on a powered supply.
