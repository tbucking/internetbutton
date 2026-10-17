# The Internet Button

## Brief

> I have plugged the device into a USB port on this machine. It's main light is flashing green.
> I want to set it up. Determine whether it needs an upgrade and how the device can be reset.

Documentation: <https://docs.particle.io/reference/discontinued/hardware/button/>

---

## 1. What the hardware actually is

The Internet Button is a **shield, not a computer**. It is two stacked boards:

- a **Particle Photon** — the actual MCU + Wi-Fi module that runs firmware, and
- the **button shield** — 11 addressable RGB LEDs, 4 push buttons, a buzzer and an accelerometer.

Taken from Particle's datasheet and from the InternetButton library source, the shield is wired as:

| Function | Photon pin | Notes |
| --- | --- | --- |
| 11 × **WS2812B** LEDs | `D3` | driven as a single-wire `Adafruit_NeoPixel` strip |
| Button 1 / 2 / 3 / 4 | `D4` `D5` `D6` `D7` | `INPUT_PULLUP`, so active low |
| Buzzer | `D0` | driven with `tone()` |
| Accelerometer | SPI | an **ADXL362** (SPI, *not* I²C — and not the LIS3DH rumoured in some places) |

Those four buttons are the **D-pad**. **None of them is MODE or RESET** — see §9, which matters
because this device's reported fault is exactly the one you fix with the MODE button.

There is **no firmware on the shield**. "Upgrading the Internet Button" therefore means
*upgrading Device OS on the Photon underneath*. The "main light" that is flashing green is the
**Photon's single RGB status LED**, seen through the diffuser.

Observed on this machine:

| Fact | Value |
| --- | --- |
| USB ID | `2b04:c006` — Particle Photon with WiFi |
| USB serial string | `00000000050C` |
| Kernel port | `/dev/ttyACM0` |
| udev symlink | `/dev/serial/by-id/usb-Particle_Photon_with_WiFi_00000000050C-if00` |
| Enumerated mode | Device OS running (bootloader intact) |

On the last row: `c006` is what the **running Device OS** presents as, and it does **not** change
between application mode and listening mode — confirmed by observation, since this device reported
`c006` while blinking green (application) and still reports `c006` while blinking blue (listening).
So `c006` tells you the bootloader is intact and Device OS is running; it says nothing about which
mode. DFU mode is the one that presents as a different device, because in DFU the bootloader is in
charge rather than Device OS.

Consequence worth knowing: **you cannot infer the installed Device OS version from `lsusb`.** There is
no version information in the USB descriptors, so the version genuinely has to be read over serial —
see §5.

Two things worth flagging:

- The USB serial string is `00000000050C`, not a normal 24-hex-digit Particle device ID. That is
  the *USB* identifier only — the real device ID has to come from `particle identify`. A mostly-zero
  serial is unusual and worth confirming (it can indicate a device whose unique ID area was never
  programmed), but it does not by itself prevent the device from running.
- The Photon is Gen 2 (STM32F205). This constrains the answer to §3.

## 2. What "flashing green" means

Per Particle's status-LED table, **blinking green = the device is trying to connect to Wi-Fi.**

> **Superseded — see §12.** The reasoning below was reasonable from the LED alone, but it reached the
> wrong conclusion. The device was not sitting on blinking green; it was **rebooting roughly every 30
> seconds**, and each reboot happened to catch the green phase. The cause was a power brownout — traced
> to a **poor USB cable** — not stale credentials. Read §12 before acting on this section.

This is *not* an error and *not* a corrupt image:

- White blink → green → cyan → breathing cyan is the normal power-up sequence. This device stops at
  green, which means it is looping on **"associate with the Wi-Fi network whose credentials I have
  stored"**.
- It never gets an IP, so it never reaches cyan (cloud). There is no red SOS pattern, so Device OS
  has not panicked and the application is not crashing.
- The ~~overwhelmingly likely~~ *originally suspected* cause was that the stored credentials point at a
  network that no longer exists, or whose passphrase changed.

~~So the device is fundamentally healthy.~~ It needs **re-provisioning**, and (see below) very probably a
Device OS upgrade.

**The lesson from getting this wrong:** blinking green looks like one state but conflates two — *"trying
to connect"* and *"rebooting so fast that green is all you ever see"*. A single LED observation cannot
distinguish them. The cheap discriminator is the USB bus: a device that is genuinely idle on green
enumerates **once**; a looping one produces a climbing device number. Always check the enumeration count
before believing an LED. See §12 for the measurement.

## 3. Does it need an upgrade?

**Treat yes as the working assumption, but measure before flashing** — the exact version is the one
fact that cannot be determined without the USB access in §4.

### Why an upgrade is very likely warranted

1. **The supported window for a Photon is narrow and low.** From Particle's *Recommended LTS
   Versions* list, Photon is `Current LTS 2.3.1`, `Minimum 1.2.1`. The earliest Photon Device OS
   release ever published was **0.4.1 (May 2015)**, so a genuinely old unit can easily be sitting on
   0.4.x or 0.5.x — three major lines behind.
2. **Photon can never exceed 2.3.1.** It appears in the *"Devices that must stay on 2.x LTS"* table,
   so 3.x/4.x/5.x/6.x are not options for this hardware. **2.3.1 is the ceiling and the target.**
3. **Old Device OS breaks modern tooling.** The `particle usb` command family (and `particle wifi`)
   only exists on **Device OS ≥ 1.0.0** for Gen 2. On an older build those commands simply are not
   there. Particle's own CLI docs point this out and say the fix is `particle update`.
4. **The automatic upgrade path is closed to you.** Device OS is normally upgraded for free by the
   *Safe Mode healer*: flash user firmware that targets a newer version and the cloud pushes the
   missing system parts. That requires a working cloud connection — which is exactly what this device
   does not have. So the upgrade has to be done **over USB**, deliberately.

### The one thing to check first

The only reason *not* to upgrade is if the device is already at/above 2.3.1, which is implausible for
something described as "very old" but is cheap to confirm. Read the version first (§5), then decide.

## 4. Blocker: USB access needs one privileged step

This cannot be worked around without a password, so it is the one step I cannot do for you.

`lsusb` sees the device and `particle serial list` can enumerate it, but **reading or writing it
fails**:

```text
$ particle usb list
You are missing the permissions to access USB devices without root.

$ particle serial identify
Could not identify device: Cannot read properties of undefined (reading 'getDevice')
```

The cause is permissions, not hardware:

| Node | Owner / mode | Consequence |
| --- | --- | --- |
| `/dev/ttyACM0` | `root:dialout` `crw-rw----` | current user is **not** in `dialout` |
| `/dev/bus/usb/001/009` | `root:root` `crw-rw-r--` | control requests (DFU, identify) refused |

No Particle udev rules are installed, and `sudo` requires a password.

**Run one of these yourself** (I have deliberately not attempted any `sudo` command, because a
password prompt cannot be answered here and secrets must never be routed through the model):

```sh
# Option A — Particle's own udev rules (recommended)
curl -fsSL -o /tmp/50-particle.rules https://docs.particle.io/assets/files/50-particle.rules
sudo cp /tmp/50-particle.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger

# Option B — group membership (requires a full log out / log in to take effect)
sudo usermod -aG dialout "$USER"
```

`particle usb configure` is the CLI's built-in equivalent of Option A.

Why Option A is the better one here, having read the rules file: its matching rule is

```text
SUBSYSTEMS=="usb", ATTRS{idVendor}=="2b04", ATTRS{idProduct}=="[cd]0??", GROUP="plugdev", MODE="0666", ENV{ID_MM_DEVICE_IGNORE}="1"
```

Three things follow:

- `2b04` + `[cd]0??` matches this device (`c006`), so it is covered.
- `MODE="0666"` makes the node world-writable, so **you do not need to join `dialout` or log out** —
  which is why Option A takes effect immediately and Option B does not.
- Because the rule matches on `SUBSYSTEMS` (any ancestor), it also applies to the derived
  `/dev/ttyACM0`, so it fixes the serial port as well as the raw USB device. Both the DFU path *and*
  the `particle serial` path are unblocked by the one file.
- `ENV{ID_MM_DEVICE_IGNORE}="1"` stops ModemManager grabbing the port, which is a common cause of
  intermittent failures on Ubuntu.

The rules file itself says to unplug and reconnect afterwards; `udevadm trigger` normally re-applies
permissions to the already-connected device without a replug, so try the trigger first. If
`particle usb list` still warns, then replug — and since replugging resets the device out of
listening mode, hold **SETUP** until it blinks blue again.

Then, in a fresh terminal:

```sh
export PATH="$HOME/bin:$PATH"     # the CLI installer added ~/bin to PATH, but this shell predates it
particle usb list
```

You should now see the Photon listed with no permission warning. That is the go/no-go check: if
`particle usb list` still warns about permissions, **fix that before going further** — everything below
depends on it.

### Toolchain status

Already present and sufficient:

- `~/bin/particle` — **Particle CLI 3.52.1** (installed during this session; the installer put it in
  `~/bin`, which `particle-install-cli.sh` appended to `$PATH` in your shell rc)
- `openssl` 3.5.5 (required by the `particle keys` commands), `node` v24.18.0, `python3`
- Google Chrome — needed for the no-install browser fallback in §6

Not present and not needed for the USB route: `dfu-util`. The CLI bundles its own DFU implementation.

### Second blocker: the CLI's USB enumeration hangs on this device

After the udev rule is installed the permissions problem is gone — confirmed by observing
`/dev/ttyACM0` and `/dev/bus/usb/001/010` become `crw-rw-rw- root:plugdev`. But `particle usb list`
then **hangs forever** (exit code 124 under `timeout`; `Ctrl-C` leaves the process alive holding the
device). This is a distinct, second problem, and it is **not** a permissions or ModemManager issue:

- `ID_MM_DEVICE_IGNORE=1` is set on the device and `mmcli -L` reports "No modems were found", so
  ModemManager is not holding it.
- `particle serial list` works (it only reads sysfs).
- `particle serial monitor` opens the tty fine.
- `lsusb -v` completes its control transfers normally, so the device *does* answer standard requests.

Root cause, from tracing the hung process and reading the library source:

1. `strace` shows the CLI opens `/dev/bus/usb/001/010`, issues `USBDEVFS_GET_CAPABILITIES`, then
   submits five control URBs. The first four complete; **the fifth is submitted and never completes**,
   and the process then sits in `epoll_wait` indefinitely. It is waiting for a reply that never comes.
2. That request is `particle-usb`'s `_getFirmwareVersion()` (`src/device-base.js`):
   `bmRequestType: DEVICE_TO_HOST`, `bRequest: PARTICLE_BREQUEST`, `wIndex: 30` (the `SYSTEM_VERSION`
   vendor request), and `this._dev.transferIn(setup)` — **with no timeout**.
3. `particle-usb` only skips such requests when the platform sets the quirk
   `controlRequestsNotSupported`. In `@particle/device-constants` that quirk is set for exactly **one**
   platform — `tachyon`. The `photon` entry has **no quirks at all**:

   ```json
   "usb": { "vendorId": "0x2b04", "productId": "0xc006" }
   ```

   So there is no code path that skips the request for a Photon.

Conclusion: **particle-cli 3.52.1 / particle-usb 4.3.1 assume every Particle device implements the
`SYSTEM_VERSION` vendor control request. This device's Device OS does not, and because the transfer has
no timeout, enumeration never returns.** `particle serial identify` is not an alternative — it calls
`usbUtils.executeWithUsbDevice(...)` and takes the firmware version from `device.firmwareVersion`, so it
needs the same request and fails fast with `Cannot read properties of undefined (reading 'getDevice')`.

Two consequences that matter:

- Any CLI command that enumerates the device in **application or listening mode** will hang:
  `particle usb list`, `particle usb dfu`, `particle serial identify`, and — if it needs to switch the
  device into DFU mode itself — `particle update`.
- The fix is to **avoid enumerating the CDC device**. In **DFU mode** the device presents as
  `2b04:d006`, a DFU-class device, not the `c006` CDC device, so the `SYSTEM_VERSION` request is never
  attempted. That is why the workaround below is to enter DFU mode *physically* and then flash.

Workarounds, in order of preference:

1. **Enter DFU mode with the buttons, then use the CLI.** Hold **SETUP**, tap **RESET**, keep holding
   through the magenta blink, release on yellow. Now the device is `2b04:d006`, and:

   ```sh
   particle update                 # or: particle update --target 2.3.1
   ```

   This sidesteps the hang because no CDC device is enumerated. Verify it actually avoided the hang
   rather than trusting it — if the command sits silent for more than ~30 s, it has hung again.

2. **Use Chrome's WebUSB restore tool** — a completely different USB implementation that does not send
   `SYSTEM_VERSION` at all, so it is immune to this bug. See §6.

3. **Install an older particle-cli** whose bundled `particle-usb` predates the `SYSTEM_VERSION`
   request. Possibly the cleanest CLI-side fix, at the cost of pinning to an old CLI.

A note on ordering: because the hang is in *enumeration*, `particle usb list` **cannot** be used as the
go/no-go check originally suggested above. The meaningful check is now `particle update` in DFU mode,
or the browser tool.

## 5. Step 1 — measure the installed Device OS version

> **Blocked as written.** Both commands below need the same USB enumeration described in §4, so on this
> device `particle identify` fails and `particle usb`-based commands hang. See "Reading the version
> anyway" at the end of this section.

Put the Photon into **listening mode** (blinking dark blue): hold **MODE/SETUP**, release when it
blinks blue. Then:

```sh
particle identify            # device ID + current system firmware version
particle serial inspect      # per-module versions and integrity/dependency status
```

`particle serial inspect` is the more informative of the two. It lists each firmware module with a
numeric version, for example a system module at `1512` and a bootloader at `502`. **These are Device OS
versions in a packed form**, and they do not read directly as `major.minor.patch` — Particle's own
example is that `1512` means Device OS `1.5.2`, which no obvious decimal expansion produces. Do not
guess the rule; look the number up in the authoritative table at
<https://github.com/particle-iot/device-os/blob/develop/system/system-versions.md>.

Read two things out of it:

- **The `System module #1` version** → the installed Device OS. If it decodes to below `2.3.1`, upgrade.
- **Any `FAIL` line.** A `FAIL` under `Dependencies` normally means the bootloader and system parts are
  mismatched. The cloud repairs this automatically, but this device is offline, so it can only be
  repaired over USB — which is precisely what §6 does.

### Reading the version anyway

`particle serial inspect` is **not** an escape route either. Reading the CLI source, `_inspectDevice`
calls `device.getFirmwareModuleInfo({ timeout: 5000 })`, which is documented in the code as
"Obtains module info from **control requests**" — the same vendor-request mechanism, over the same
`usbUtils.executeWithUsbDevice(...)` enumeration that hangs. So both `identify` and `inspect` are
blocked by §4. (It is at least evidence that Particle moved module info from serial to control
requests: an older CLI read it over the serial port in listening mode, which is why the "older CLI"
workaround in §4 also restores the ability to measure.)

So the realistic options are:

- **Flash first, measure never.** `particle update --target 2.3.1` in physical DFU mode sets the
  version to 2.3.1 regardless of what was there. Since **2.3.1 is the ceiling for this hardware**,
  there is no version decision to make — measuring first was only ever a way to avoid an unnecessary
  flash, not a prerequisite for a correct one. This is the pragmatic choice.
- **The browser restore tool** in §6, which does not use control requests at all and reports the
  device's firmware information in its support panel.
- **An older particle-cli**, which reads both device info and module info over the serial port instead
  of control requests. If you specifically want the before/after version numbers, this is the cleanest
  way to get them.

## 6. Step 2 — upgrade Device OS over USB

> **This section is the plan; see §11 for what actually worked.** In short: `particle update` needs a
> login, and its mid-flight "switch to normal mode" step failed on this device. Flashing the official
> restore binaries directly with `particle flash --usb` — which needs **no login** — is what did the job.

`particle update` "will update to the latest LTS supported by the device" — for a Photon that is
**2.3.1**. It also repairs a mismatched bootloader. Put the device in **DFU mode** first (blinking
yellow): hold **MODE/SETUP**, tap **RESET** while holding, keep holding through the magenta blink,
release on yellow.

```sh
particle update                       # → 2.3.1 for Photon
# or pin it explicitly:
particle update --target 2.3.1
```

Then flash known-good user firmware, which also confirms the upgrade took:

```sh
particle flash --local tinker
```

`particle update` does **not** touch the Wi-Fi credentials, so afterwards the device will still be
blinking green until §7.

**No-install alternative.** If you would rather not use the CLI, Chrome is installed and Particle's
browser tool `https://docs.particle.io/tools/device-restore/device-restore-usb/` works on Gen 2 devices
and can flash Device OS, bootloader and Tinker purely through WebUSB — there is a *Specific Device OS
version* selector under **Advanced Options**, so you can pin 2.3.1. Caveats for this route: Chrome /
Edge / Opera only (Firefox and Safari will not work), it still needs the udev rule from §4, and its
Wi-Fi setup step does **not** support the Photon — use the CLI for Wi-Fi either way.

## 7. Step 3 — provision Wi-Fi ("set it up")

Listening mode → send credentials:

```sh
particle serial wifi
```

On Gen 2 the Wi-Fi **scan runs on your computer's** Wi-Fi interface, not on the device — Gen 2
hardware has no useful scan-from-device path, so make sure your machine's Wi-Fi is on, otherwise the
network list will be empty and misleading. If the network is hidden, or you are on WPA2-Enterprise, use
`particle serial wifi --file credentials.json`:

```json
{ "network": "my_ssid", "security": "WPA2_AES", "password": "my_password" }
```

Success looks like: blinking green → blinking cyan → **breathing cyan**. That is "connected to the
Particle cloud" and is the real finish line.

Note `particle wifi add` / `particle wifi join` are *not* available here — those need Device OS 5.8.2+,
and a Photon cannot run them. `particle serial wifi` is the correct command for this hardware.

## 8. Step 4 — verify the shield actually works

Breathing cyan only proves the Photon works. It says nothing about the 11 LEDs, 4 buttons, buzzer and
accelerometer. Now that the device is claimed (§13) this can be done **over the air** rather than over
USB.

The Internet Button is discontinued, but Particle's official library is still published and the
library archive still serves it. A ready-made project is checked in at `firmware/shield-check/`:

```sh
cd firmware/shield-check
particle compile photon . --target 2.3.1 --saveTo shield-check.bin
particle flash 1f0034000e47343432313031 shield-check.bin
```

**`--target 2.3.1` is not optional.** See §6 — the same command without it produced a binary depending
on Device OS **3.3.1**, which is precisely what triggered the Safe Mode healer.

A second, larger project lives alongside it: `firmware/interval-timer/` is a working pomodoro-style
timer that uses the 11 LEDs as a progress bar, with buttons, buzzer and the accelerometer for
auto-pause. It has its own README. It is the practical proof that the shield's peripherals are all
usable, and it reuses the calibration and library gotchas recorded in this section.

### Two CLI commands hang here, so the project was built by hand

`particle project create` and `particle library add` both hung with no output and created nothing
(observed as exit code `124` from `timeout`). The project was therefore assembled manually —
`project.properties` holding `name=shield-check`, sources in `src/`, library in `lib/InternetButton/`
— which the compiler accepts without complaint. Worth knowing before assuming the tooling is broken.

### Split the check: what needs a human, and what does not

The four peripherals separate cleanly. LEDs and buzzer need eyes and ears, but **the buttons and the
accelerometer are electrical**, so the firmware exposes them as Particle variables and they can be
read back and checked:

| Peripheral | How it is checked | Result |
| --- | --- | --- |
| LEDs | `Particle.function("sweep")` walks LEDs 1–11 | **working** — all 11 lit in order, then all together |
| Buttons | `Particle.variable("seen")` → latched bitmask | **working** — all four registered, `seen = 15` |
| Accelerometer | `Particle.variable("accel")` → `"x,y,z"` | **working** — reads gravity, and fluctuates |
| Buzzer | `Particle.function("beep")` plays a note | **working** — clear tones (my first verdict was wrong; see below) |

The accelerometer is the interesting one, because a *plausible* number is not evidence. At rest:

```text
$ particle get 1f0034000e47343432313031 accel
-7,0,72
```

In the ADXL362's 8-bit 2 g mode one gravity ≈ 64 LSB, so `72` on Z means **the sensor is reading
gravity**, and X/Y near zero means the board is lying flat. That is the correct signature of a working
particle. To rule out a stub returning a constant, sample it repeatedly:

```text
-7,0,72    -7,-1,72    -7,0,73    -7,0,72    -7,0,72    -7,0,72
```

Three distinct readings in six, Z moving `72..73` — that is sensor noise, and a frozen constant would
have been the suspicious result. The residual X offset near `-7` (≈ 0.11 g) is a zero/mounting offset,
not a fault.

### The first button test was wrong, and the instrument was to blame

The first attempt polled `buttons` — the *currently held* mask — in a loop from the cloud, and caught
**one press in 80 seconds**. The tempting reading is "three of the four buttons are dead". That reading
is wrong, and it is the same failure recorded in §12, wearing different clothes: the poll sampled at
about 1 Hz, a human tap is shorter than that, and the misses were invisible. A probe that cannot see
the thing under test returns the same answer for a working button and a broken one, which is the
definition of a useless check.

The fix was to **change the instrument, not the conclusion.** The firmware now latches every up→down
edge in a loop running every 20 ms, counting presses per button and setting a bit in a `seen` mask.
Because the latch persists, a press no longer has to coincide with a poll — which also removes the need
to synchronise a human with a script. With that instrument, all four buttons register at once:

```text
baseline  seen = 0    counts = 0,0,0,0
    6.6s  seen = 12   counts = 0,0,1,1
    8.6s  seen = 15   counts = 1,1,1,1
   38.8s  seen = 15   counts = 3,3,5,4
FINAL  seen = 15 (1111)
```

Two things worth keeping: **a null result from a slow probe is not evidence of absence**, and when a
measurement contradicts expectation the instrument is the prime suspect — the same rule §12 arrived at
from the opposite direction.

### The rest, by eye and ear

The firmware boots into **button mode**: any held button lights the LED paired with it, so one press
tests both. Press each of the four in turn — the nearest LED should light while it is held.

```sh
# LEDs
particle call 1f0034000e47343432313031 sweep      # LEDs 1..11 one at a time, then all together
particle call 1f0034000e47343432313031 led red    # all LEDs red; also off/white/green/blue/1..11
# buzzer
particle call 1f0034000e47343432313031 beep C5    # short tone
# buttons — `buttons` is live, `seen` and `counts` are latched since the last reset
particle call 1f0034000e47343432313031 reset
particle get  1f0034000e47343432313031 buttons    # bit0..bit3 = buttons held right now
particle get  1f0034000e47343432313031 seen       # bit0..bit3 = buttons pressed at all
particle get  1f0034000e47343432313031 counts     # "c1,c2,c3,c4" press counts
```

Read `seen`, not `buttons`, when a human is doing the pressing: `seen` reads `15` once all four have
been pressed at any point, so the check does not depend on catching a press in the act.

Which LED pairs with which button: the library's own example pairs `buttonOn(2)` with `ledOn(3)`, so
button *i* lights LED *i+1*.

Note `b.begin()` vs `b.begin(1)`: the `1` is for the original SparkButton, which lacks the buzzer and
enclosure. This unit is an **Internet Button**, so it is plain `b.begin()`. Verified in the library
source — the no-argument `begin()` leaves the ring on `D3` and the buttons on `D4`–`D7` as
`INPUT_PULLUP`, matching §1.

Flashing this **replaces Tinker**, which is expected: Tinker cannot drive WS2812B pixels, since they
need bit-banged NeoPixel timing rather than a plain `digitalWrite`.

To put Tinker back, **flash the Tinker binary from the 2.3.1 release rather than asking the CLI for
`tinker`.** `particle flash <device> tinker` fetches whatever the CLI considers current, and a Tinker
built for a newer Device OS would pull the very upgrade §6 is about. The restore set used here is the
official 2.3.1 download, and its `tinker.bin` is checked as `2302` *before* flashing:

```sh
particle binary inspect /tmp/restore231/tinker.bin   # → system module number 2 at version 2302
particle flash 1f0034000e47343432313031 /tmp/restore231/tinker.bin
```

Afterwards the device reports `system_firmware_version: 2.3.1` and Tinker's
`digitalread`/`digitalwrite`/`analogread`/`analogwrite` answer again — so the swap is verified, not
assumed.

### The cloud's view of the device is cached, so probe the device

One more measurement trap, caught here. Immediately after the Tinker flash the device record still
listed the *shield-check* functions and variables, and immediately after the shield-check flash it
still listed *Tinker's*. Reading that record suggests the flash silently failed. It had not.

The discriminating test is to **talk to the device rather than the registry**:

```text
--- shield-check probes ---
  variable seen    -> absent (HTTPError)
  variable counts  -> absent (HTTPError)
--- Tinker probes ---
  function digitalread   -> 0
  function digitalwrite  -> 1
```

Absent variables plus answering Tinker functions is unambiguous, and a moment later the cached record
caught up. **A stale index is not evidence about the thing indexed** — the same lesson as §11's
`particle list` and §13's ownership field, in a third guise.

The buzzer deserves its own note, because my first verdict on it was **wrong** and the correction is
instructive.

**What I reported first:** "working, but faint and slightly crackly", and I explained it as a small piezo
sharing a 5 V rail with 11 LEDs — i.e. a hardware/power characteristic.

**What was actually true:** the buzzer was fine, and my *test* was broken. `playNote`'s second argument
is not a duration in milliseconds:

```cpp
if (duration != 0) duration = 1000*240/bpm/duration;   // bpm defaults to 250
...
tone(D0, int(freq), duration); delay(duration); noTone(D0);
```

So `b.playNote("C5", 400)` plays for `960/400` = **2.4 ms**. That is a click, not a tone — which is
precisely what "faint and slightly crackly" describes. The library's own examples pass `8` (= 120 ms).
With `4` (= 240 ms) the tones are clear, and that was confirmed by ear.

The lesson is the one this document keeps relearning: **a plausible-sounding physical explanation is not
a diagnosis.** I had a tidy story about rail sag (§12's brownout) that fit the symptom perfectly, and it
made me stop looking at my own code. The symptom was reproducible and *cheap to re-test* — one argument
change — and I never re-tested it before writing it up as a hardware finding. Note also that the wrong
explanation borrowed credibility from a real finding nearby, which is what made it feel solid.

Two further gotchas in the same function, both silent failures:

- `playNote` **blocks** for the note's length (`delay`), so a chime stalls the caller. Keep chimes short
  inside any timing loop.
- It calls `tempNote.toUpperCase()` and **discards the result**, then switches on the original case, so
  note names must be given in uppercase. A lowercase `"c5"` matches no `case` and plays nothing.

**The shield silkscreen is wrong.** Particle documents this explicitly: the four button labels are
printed in the *reverse* order from the truth. Reading right to left they should be `1-2-3-4`, but the
PCB is annotated `4-3-2-1`. The real mapping is:

| Photon pin | Button number | Silkscreen says |
| --- | --- | --- |
| `D4` | Button 1 | Button 4 |
| `D5` | Button 2 | Button 3 |
| `D6` | Button 3 | Button 2 |
| `D7` | Button 4 | Button 1 |

So if a button appears to do the wrong thing, suspect the silkscreen before your code. Note this also
means the four shield buttons are **not** labelled `SETUP`/`RESET`/`MODE` in any orientation — they
carry only these numeric button labels (see §9).

`5_Motion` and `6_Orientation` test the **ADXL362 accelerometer**, which is on SPI. If motion readings
are dead while LEDs and buttons work, that points at the accelerometer or its SPI wiring specifically.

## 9. How to reset the device

There are several distinct things called "reset". They are not interchangeable:

| Goal | Gesture | LED when you release |
| --- | --- | --- |
| **Reboot** — keep everything, just restart | tap **RESET** | normal power-up sequence |
| **Listening mode** — accept config over USB/serial | hold **MODE**, release on blue | blinking dark blue |
| **Network reset** — erase stored Wi-Fi credentials | hold **MODE** until it blinks blue, **keep holding ~10 s more** until it blinks blue *rapidly*, then release | fast blinking blue |
| **Safe mode** — connect to cloud but do **not** run your app | hold **MODE**, tap **RESET**, release on magenta | breathing magenta |
| **DFU mode** — reflash Device OS / user firmware over USB | hold **MODE**, tap **RESET**, keep holding past magenta, release on yellow | blinking yellow |
| **Bootloader rescue** — status LED never lights at all | hold **MODE** while plugging USB in | — |
| **Factory reset** | **not applicable — unsupported on Photon 1 / P1** | — |

The one you actually want for the reported problem is the **network reset** (third row). It clears the
stale Wi-Fi credentials that are causing the green blink, after which the device sits in listening mode
ready for §7. On Photon 1 it clears the Wi-Fi credentials but deliberately **not** the antenna
selection.

Three gestures from Particle's generic table do **not** apply to this hardware, so do not go looking
for them: **factory reset** (explicitly unsupported on Photon 1 / P1), **soft power off** (tap MODE
twice), and the **signal-strength readout** (tap MODE once). All three require Gen 3+ or newer.

### The trap: listening mode is not in the boot menu

These two gestures look similar and are **not** interchangeable:

| You want | Gesture | Requires |
| --- | --- | --- |
| **Listening mode** (blinking blue) | hold **SETUP alone** | the device already **running** |
| **Safe mode / DFU / factory reset** | hold **SETUP**, *tap* **RESET** | the **boot menu**, at power-on |

Holding SETUP from power-on walks the **boot menu**: magenta (safe mode) → yellow (DFU) → white
(factory reset). **Blue never appears in that sequence**, and on Photon 1 the final white step is an
unsupported factory reset — so holding "for a long time" from boot yields nothing but a reset.

Listening mode is a *runtime* mode, not a boot mode. Release everything, let the device boot and settle
into blinking green/cyan, and only *then* hold SETUP. Per Particle: "This typically takes a couple
seconds, but could take longer, especially if the device is currently blinking green."

Two consequences worth knowing:

- **Every tap of RESET reboots the device.** Repeating "hold SETUP, tap RESET" produces a reboot loop —
  observed here as **16 USB re-enumerations in about three minutes**. A device rebooting every few
  seconds can never settle into listening mode, so that gesture actively prevents the thing you are
  trying to reach. If `journalctl -k | grep 'usb 1-9'` shows device numbers climbing, you are in this
  state.
- **Dark blue is dim.** Photon listening-mode blue is much dimmer than the other colours and can look
  almost black. Watch for a slow dim pulse, not a bright blue.

If the device is in DFU (blinking yellow), the button does nothing useful — DFU is bootloader code with
no button handling. Tap RESET once, wait for the device to boot and settle, and only then hold SETUP.

### No button-free route on this firmware

It would be convenient to enter listening mode from the CLI, but **`particle usb start-listening` does
not work on Device OS 2.3.1** — it hangs forever, exactly like the enumeration bug in §4, because the
listening-mode control request postdates 2.x. Verified by test after the upgrade, with `timeout`
reporting exit 124.

**But DFU mode *is* reachable without the button** — corrected after testing. `particle usb dfu`:

- **succeeds at switching the device**: it re-enumerates from `2b04:c006` to **`2b04:d006` DFU**, every
  time, verified by `lsusb`;
- **then hangs**, because the CLI's event loop never returns (exit 124 under `timeout`).

So the command *does the job* and only fails to exit. Run it under `timeout` and check `lsusb` rather
than trusting the exit code:

```sh
timeout 20 particle usb dfu     # will report 124; that is expected
lsusb | grep 2b04               # must show 2b04:d006 (DFU Mode)
```

That removes the button from the DFU path. Combined with `particle flash --usb <file>`, the whole
upgrade path in §11 can be driven without touching the hardware.

### Holding RESET: the LED going out is correct

If you hold **RESET**, the status LED turns **off** and stays off until you let go. This looks alarming
and is entirely normal:

- RESET is wired to the microcontroller's **NRST** pin. While you hold it, the MCU is held in reset.
- The RGB status LED is **driven by the MCU's own GPIO pins**. With the MCU halted, those pins are
  high-impedance and the LED simply has no drive — so it is dark.
- Release, and the MCU boots: white blink → green → cyan.

Consequences:

- **RESET is a tap, never a hold.** There is no gesture that involves holding RESET on its own. Hold it
  down and you are only keeping the chip switched off.
- The only time you hold anything *through* a reset is the boot menu: hold **SETUP**, *tap* **RESET**,
  then release SETUP at the colour you want.
- If you want a reboot: tap RESET and let go immediately, then leave the device alone for ~15 s.

So "LED off while RESET is held" is not a symptom of the brownout loop in §12 and not a fault. It is the
expected electromechanical behaviour of a reset button.

Also note the naming: the Photon's two buttons are silkscreened **SETUP** and **RESET**. Particle's docs
call SETUP "MODE". They are the same button.

### Which button is MODE/SETUP?

**None of the four buttons on the bottom of the shield.** Those four are the D-pad, wired to `D4`–`D7`
and read in code as `b.buttonOn(1..4)`. They are inputs to *your* application; they have no role in
booting, and holding them at power-up does nothing.

**MODE/SETUP and RESET are two tiny switches on the Photon itself**, not on the shield. The Photon
datasheet's bill of materials confirms they are the *only* two switches on that board — `SETUP` and
`RESET`, each a 3.6 mm × 3.1 mm surface-mount tact switch. Both are labelled in white silkscreen, and
that silkscreen is the reliable identifier — not position. They are conventionally located next to the
micro-USB connector. To find them:

1. Locate the **micro-USB connector** (the cable you already have plugged in) — it is on the Photon,
   protruding from the assembly.
2. Look on the Photon's PCB near that connector for two small silver squares. They are far smaller
   than the four round buttons on the shield, and much closer together.
3. Read the silkscreen: it says `SETUP` and `RESET`. `SETUP` is MODE.

Practical notes:

- The clear plastic **module cover** sits over the Photon, so you may have to lift it off to get at
  both switches. It is designed to come off.
- They are small and recessed; a fingernail, the tip of a pen or a small flat screwdriver is the usual
  way to press them. You need to hold one *while* tapping the other, so make sure you can reach both
  before you start.
- The Photon is the rectangular board in the middle. If you are looking at four big round buttons and
  nothing else, you are looking at the back of the shield, not at the Photon.

### Why you cannot skip this on an old device

The CLI can drive all of these over USB without touching the buttons:

```sh
particle usb start-listening    # listening mode
particle usb dfu                # DFU mode
particle usb safe-mode          # safe mode
particle usb reset              # reboot
```

**But** those commands only exist from **Device OS 1.0.0** onward on Gen 2 (Photon/P1); on older
builds they are simply absent. That is the whole point of §3: if this Photon predates 1.0.0, then the
**physical SETUP/RESET buttons are the only route into DFU mode**, and therefore the only way to
perform the upgrade at all. The chicken-and-egg is real — you need the buttons to upgrade, and you need
to upgrade to avoid needing the buttons.

So: find them first, before you need them. Confirm you can press `SETUP` and `RESET` independently and
see the LED respond (holding SETUP alone should get you to blinking dark blue). If you cannot reach
them, solve that before anything else in this document.

### Recovering from a bad flash

If a flash goes wrong and the device is stuck in a red-blink SOS loop, **safe mode** (fourth row) is
the escape hatch: it boots Device OS without your application, which tells you whether the fault is in
your firmware or in the system parts. Recovery *over the air* from safe mode still needs working Wi-Fi,
so while this device is offline use the USB route instead:

```sh
particle flash --local tinker
```

That is the reliable "get back to something known-good" command, and it works without any network.

## 10. Summary

- **What it is:** a Photon + Internet Button shield. The shield has no firmware; only the Photon does.
- **What green means:** "I can't reach my Wi-Fi network." Healthy device, stale credentials. Not a fault.
- **Upgrade:** yes, almost certainly. Photon's supported range is **1.2.1 → 2.3.1**, and 2.3.1 is the
  hard ceiling for this Gen 2 hardware. Old builds additionally lack the `particle usb` commands.
- **Measure first:** `particle serial inspect` in listening mode, and decode the module versions
  against `system-versions.md`.
- **Blocker:** USB permissions. Run the udev-rule step in §4 yourself, then `particle usb list` must
  come back clean.
- **Upgrade:** DFU mode → `particle update` (→ 2.3.1) → `particle flash --local tinker`.
- **Reset:** the procedure you want is the **network reset** — MODE held through blue for ~10 s more
  until it blinks blue rapidly. There is no factory reset on this hardware.
- **Which button is MODE?** None of the four on the bottom of the shield — those are the D-pad on
  `D4`–`D7`. MODE/SETUP is a tiny switch on the **Photon** next to its micro-USB socket, beside RESET,
  labelled in silkscreen. On a pre-1.0.0 device those physical buttons are the *only* way into DFU, so
  confirm you can reach them before you start. See §9.
- **Finish:** `particle serial wifi` → breathing cyan, then flash the checked-in
  `firmware/shield-check/` project (§8) to prove the LEDs, buttons, buzzer and accelerometer work.
  Compile it with `--target 2.3.1`, or it will silently upgrade the device instead (§6).
- **Ownership:** a device can be *working* and still belong to somebody else. `particle list` only
  shows devices in your own account, so its silence proves nothing (§13).

## 11. Outcome — what actually happened, verified

Recorded after the fact, because the plan above differs from reality in two places.

### Result

| | Before | After |
| --- | --- | --- |
| Device OS | older build; **did not implement the `SYSTEM_VERSION` control request** | **2.3.1** (verified: `particle serial identify`) |
| Device ID | unknown (USB serial descriptor showed `00000000050C`) | **`1f0034000e47343432313031`** |
| USB mode | `2b04:c006` normal, blinking green | `2b04:c006` normal, booting cleanly |

`particle serial identify` now reports:

```text
Your device id is 1f0034000e47343432313031
Your system firmware version is 2.3.1
```

### The sequence that worked

1. **udev rules** (as §4) — cleared the permissions failure. Necessary but not sufficient.
2. **Entered DFU mode with the physical buttons.** The device re-enumerated as **`2b04:d006`
   (DFU class)**, and `/dev/ttyACM0` disappeared.
3. **Flashed the official restore binaries directly** — no login required:

   ```sh
   curl -fsSL -o photon.zip https://docs.particle.io/assets/files/device-restore/2.3.1/photon.zip
   unzip photon.zip
   particle flash --usb system-part1.bin
   particle flash --usb system-part2.bin
   particle flash --usb tinker.bin
   ```

4. After the upgrade the device **booted normally**, and the commands that used to hang came back
   instantly.

### Confirmation of the root cause

This is the cleanest evidence available, because it is the *same command* on the *same device*:

| State | `particle usb list` |
| --- | --- |
| old Device OS | hangs forever (exit 124 under `timeout`) |
| **in DFU mode** | returns instantly → `<unknown> [00000000010c] (Photon, DFU)` |
| **after upgrade to 2.3.1** | returns instantly → `<unknown> [1f0034000e47343432313031] (Photon)` |

So the hang was exactly as diagnosed in §4: the running firmware did not answer the `SYSTEM_VERSION`
vendor request, and `particle-usb` waits for it with no timeout. Fixing the firmware fixed the tooling.
Two corollaries:

- **DFU mode is the escape hatch**, because the CDC device does not exist there — the request is never
  sent. That is what let us get at the device at all.
- Other control-request commands **still fail on 2.3.1**, because they are newer than the firmware:
  `particle serial inspect` returns `Request timeout`, and `particle usb start-listening` hangs. Do not
  expect CLI-3.x-era control requests to work against 2.x firmware; only `identify` (and hence
  `SYSTEM_VERSION`) is old enough.

### Two corrections to the plan

**1. `particle update` is not the offline path, and its mid-flight mode switch failed.**
It needs a login (it downloads the binaries), and against this device it did:

```text
Flashing photon-system-part1@2.3.1.bin
Flashing photon-system-part2@2.3.1.bin
Switching device to normal mode
Flash failed.
Unable to reconnect to the device.
```

The system parts flash fine over DFU; the failure is the *transition to normal mode*, which is where the
bootloader would be written. This is recoverable and did **not** brick anything — the device booted
afterwards — but it means the last step of `particle update` is unreliable on a device this old.

**2. The bootloader was never explicitly flashed — and did not need to be.**
Particle's table says the Photon bootloader must be written in **normal mode**, not DFU (`particle
flash --usb bootloader.bin` refuses by design; the rule is hardcoded), and the manual path that used to
do it — `particle flash --serial` — no longer exists in CLI 3.52.1 (it just redirects to the DFU path).

Despite that, the device booted on 2.3.1 with no `FAIL` state, which proves the existing bootloader was
already **≥ 1003** — the first version capable of running 2.x. The bootloader requirement I flagged as a
risk turned out to be satisfied. Nothing further is needed: Particle documents that **the cloud updates
the Photon/P1 bootloader automatically once the device is online**, so connecting to Wi-Fi in §7
completes that too.

A note on measurement: the bootloader version could **not** be read before the upgrade by any means —
`lsusb`'s `bcdDevice` is a fixed `2.00`, and every CLI route needs either a login or a bootable device.
It was inferred after the fact from the device booting successfully. Worth knowing if this is ever
repeated: you may have to flash in order to find out.

### The trap: the default compile target is NEWER than a Photon can support

This one caused a real, unintended Device OS change. **Always pass `--target` when compiling for a
Photon**, because the CLI default is not what Particle's Photon documentation recommends.

Compiling with no target:

```sh
particle compile photon app.ino --saveTo app.bin     # ← DANGEROUS
```

produced a binary declaring:

```text
listen.bin ... It depends on a system module number 2 at version 3302
```

**`3302` is Device OS 3.3.1** — the CLI's default, and three minor versions above the `2.3.1` that
Photon's own documentation recommends. Flashing that user firmware onto a device running 2.3.1 sets
off the **Safe Mode healer** described in §3 as an advantage:

> If you flash user firmware that requires a newer version of Device OS than is currently installed,
> the device will automatically upgrade… the cloud will send down the missing binaries.

So the sequence was:

1. User firmware depending on `3302` was flashed over USB.
2. The device got Wi-Fi credentials and **connected to the cloud**.
3. The healer saw the dependency and **upgraded the device to Device OS 3.3.1** over the air.

The device was then reading `3.3.1` — the version jumped with no USB flashing involved. **A USB-only
mental model is wrong**: the moment the device reaches the cloud, the cloud can change its Device OS
based on what your user firmware asks for.

Rules that follow:

- **Always target explicitly**: `particle compile photon app.ino --target 2.3.1`.
- **Check before flashing**: `particle binary inspect app.bin` and confirm the `system module number 2`
  version is one your device can run (`2302` = 2.3.1 for a Photon).
- **A version that changes "by itself" is the healer**, not corruption. If a device reads a different
  Device OS after gaining network access, look at the installed user firmware's declared dependency.
- Note the *benefit* of the same mechanism: this is also how the **bootloader** gets updated on a
  Photon/P1, which as §11 explains cannot be done over DFU.

### Reproduced deliberately, side by side

The `3302` reading above was originally observed as a side effect of an accident. It was later
reproduced on purpose, compiling the *same source* twice during the shield check (§8) — which turns a
post-hoc inference into a controlled result:

```text
$ particle compile photon . --target 2.3.1 --saveTo sc-231.bin
Targeting version: 2.3.1
Compile succeeded.  Flash 20392   RAM 1584
$ particle binary inspect sc-231.bin
 It depends on a system module number 2 at version 2302     ← 2.3.1, runnable

$ particle compile photon . --saveTo sc-default.bin
Compile succeeded.  Flash 20088   RAM 1312
$ particle binary inspect sc-default.bin
 It depends on a system module number 2 at version 3302     ← 3.3.1, NOT runnable
```

Same source, same library, two binaries. Two useful tells fall out of this:

- **`--target` announces itself.** Only the targeted run prints `Targeting version: 2.3.1`. If that
  line is missing from a build, the flag was not applied and the binary is almost certainly `3302`.
- **The memory figures differ** (`20392/1584` vs `20088/1312`), which is independent evidence that these
  are genuinely different builds rather than the same artifact re-reported.

The device was then flashed with the `2302` binary over the air and came back reporting
`system_firmware_version: 2.3.1` and `status: normal` — i.e. **no healer action**. That is the positive
control: the same procedure that once caused the upgrade, done correctly, does not.

### What remains

Nothing. Every step planned in §1–§8 has now been carried out and verified:

| Step | Status |
| --- | --- |
| Measure and upgrade Device OS | done — **2.3.1**, read back from the device |
| Wi-Fi provisioning (§7) | done — the device reaches the cloud |
| Shield verification (§8) | done — all four peripherals confirmed |
| Ownership (§13) | done — claimed to `tonybuckingham@proton.me` |

### Final state

| | Result |
| --- | --- |
| Device OS | **2.3.1** (read back from the cloud *and* via `particle serial identify`) |
| Device ID | `1f0034000e47343432313031` |
| Serial | `BTTN-150809-ENGW-0` — a genuine Internet Button kit |
| Owner | **`tonybuckingham@proton.me`** |
| USB | `2b04:c006`, device number stable |
| Stability | **no restarts** — the brownout loop (§12) is gone |
| Wi-Fi | **working** — device reaches the Particle cloud |
| LED | **breathing cyan** |
| Shield | **LEDs, buttons, accelerometer and buzzer all verified** (§8) |
| Firmware | bare **Tinker**, restored from the official 2.3.1 release |

The LED reaching **breathing cyan** is the real end of the original task: the device that started out
unable to connect is now on the network and in the cloud.

**Correction to the paragraph above.** §11 first reported that the device "is not in anyone's account",
inferred from `particle list` printing `No devices found`. That inference was wrong, and trying to
claim it proved it wrong — the CLI answered, in its own words:

```text
That device belongs to someone else. Would you like to request a transfer?
Failed to claim device: That belongs to someone else.
To request a transfer add ?request_transfer=true to the URL.
```

`particle list` only lists devices claimed to *your* account, so it is silent for a device owned by
anybody. **"Not mine" is not evidence of "not anyone's"** — the same shape of error as §12's
measurement pitfalls. Details in §13.

### Version history on this device, for reference

| When | Device OS | How |
| --- | --- | --- |
| initial | pre-1.0.0 (no `SYSTEM_VERSION` control request) | as received |
| after §11 | **2.3.1** | flashed restore binaries over DFU |
| transiently | **3.3.1** | cloud Safe Mode healer, triggered by a mis-targeted compile |
| final | **2.3.1** | restore binaries re-flashed over DFU |

That third row is the whole lesson of the subsection above: a Device OS version change can happen with
**no USB activity at all**, and the trigger is the dependency declared by whichever *user* firmware the
device happens to be running.


## 12. The real fault: a brownout reboot loop

Found after the upgrade, and it revises §2. **The device reboots on a roughly 30-second cycle.**

### The symptom, and why white is the tell

A **single** white blink is normal — Particle's documented power-up sequence is *white blink → blinking
green → blinking cyan → breathing cyan*. But white that **keeps coming back** is not: white means
"reboot". So the sequence being observed is not "boots once, then can't reach Wi-Fi" but:

```text
boot (white) → trying Wi-Fi (green) → power fails → reboot (white) → ...
```

### Measuring it

The LED shows one state; the USB bus shows the truth. Every reboot re-enumerates, and the kernel
assigns a **new device number each time** — so a climbing number is proof of a loop:

```sh
journalctl -k --since "10 min ago" --no-pager | grep 'usb 1-9: new full-speed USB device number'
```

Observed here — **18 re-enumerations in 12 minutes**, average uptime between reboots **30 s** (range
2 s to 203 s):

```text
16:25:36 -> 16:26:01   +25s
16:26:01 -> 16:26:20   +19s
16:26:20 -> 16:27:01   +41s
16:27:01 -> 16:27:04    +3s
16:27:04 -> 16:27:22   +18s
```

The device number reached 33 and was still climbing. In a healthy state it enumerates **once** and stays
put.

### What it explains

Two things that looked like separate problems were both this one fault:

- **Listening mode was unreachable.** The device resets every ~30 s, and listening mode needs the device
  *running* while you hold SETUP. It never stays up long enough.
- **It never connected to Wi-Fi**, and likely never had — which is why the original "blinking green"
  symptom was never about credentials at all (§2). The device may have been looping since it was
  plugged in.

### Diagnosed as a power brownout

Particle's status-LED table is explicit for this exact combination:

> Blinking green is connecting to Wi-Fi or cellular. **A white blink indicates reboot.** This typically
> indicates **insufficient power causing the device to brown out while connecting to the network.**

Why power, and not a software crash: a crash produces a **red SOS pattern**. None is seen. A reboot with
no red, occurring while the Wi-Fi radio is transmitting, is the documented brownout signature. The
Photon's transmit peaks are **235–430 mA** (datasheet), and Particle warns that a poor cable causes
voltage sag: *"peak currents drawn by the Photon when transmitting and receiving will result in voltage
sag at the input which may cause a system brown out or intermittent operation."*

The Internet Button makes the budget worse — it adds **11 WS2812B LEDs** (up to ~60 mA each at full
white, ~660 mA for the ring) plus a buzzer, all sharing the USB 5 V rail.

### Fix, cheapest first

1. **Change the USB cable.** This is the single most likely cause and the first thing to try. Thin,
   long or charge-only cables have enough resistance that transmit peaks sag the rail. Use a short,
   thick, data-capable cable. **This was the actual cause here** — the other steps were not needed.
2. **Change the power source.** The Photon datasheet calls for a supply able to source **1 A**. A
   **powered hub**, or a 5 V phone charger (≥ 1 A, ideally 2 A), removes the host port from the
   equation entirely — this is the most reliable fix if the port is current-limited.
3. **Isolate the shield.** Unplug the Internet Button shield and power just the bare Photon. If the loop
   stops, the shield's load is implicated. The Photon runs fine standalone.
4. **Then** re-measure with the `journalctl` command above. The loop is fixed when the device number
   stops climbing. Only then will listening mode (§9) and Wi-Fi setup (§7) work — so this must be
   solved *before* anything else.

Do not attempt the Wi-Fi step until the loop is gone; it cannot succeed while the device is resetting.

### Resolution — confirmed fixed

**The cause was the USB cable.** Swapping it for a better one stopped the loop — step 1 of the list
above, and nothing else was changed.

That is worth more than "it works now", because it **rules out** the other two candidates. Had the port
been current-limited or the supply insufficient, a cable swap would not have fixed it — the deficit
would have persisted through any cable. So:

- The host port and the power source can supply the load, including the shield.
- The fault was **cable resistance** specifically: enough voltage sag under the Photon's 235–430 mA
  transmit peaks to cross the brownout threshold.
- Therefore the 11-LED ring is **within budget** on this setup. Worth knowing before writing firmware
  that drives all 11 at full white (~660 mA) — that is the one case that could push it back over.

Correctly measured:

| | While faulty | After the fix |
| --- | --- | --- |
| Interval between restarts | median **26 s**, 90th pct **213 s**, worst **439 s** | none |
| Quiet period | never exceeded ~3.5 min | **9.6 min and counting** |
| Device number | climbing every cycle (reached 33) | **stable at 034** |
| Cable | original | **replaced** — the only change |

9.6 minutes is **22× the median** fault interval and **2.7×** the 90th-percentile gap, and a continuous
**300-second watch recorded zero USB events**. That is the standard to meet: the quiet period must
exceed every gap seen while the fault was active, or it proves nothing (see below).

One caveat on the attribution, in keeping with the rest of this section: this is a *single* swap with no
reversal. The fix correlates with the cable, and the reasoning above makes a cable cause much the most
plausible, but strictly the test that would nail it is putting the **original cable back** and watching
the reboots return. That was not done — the device was needed, and re-creating a fault to prove a fix is
seldom worth it. Recorded as strong inference, not the same grade of proof as the version read-back.

### Measurement pitfall — how I got this wrong twice

Worth recording, because BOTH errors produced a confident wrong answer:

1. **Counting only one event class.** I grepped `new full-speed USB device number` and computed "time
   since the last reboot" from it. But the kernel logs a **`USB disconnect, device number N`** line as
   well, and the true last restart was a *disconnect* at `16:30:52` — invisible to that grep. Using the
   wrong event class gave a quiet period of 6 min 39 s when the real figure was 3 min 17 s — **less than
   the 203 s worst case already observed**, so it was inconclusive rather than proof. Always match
   `usb N-M: (USB disconnect|new full-speed)` together.
2. **Trusting a device number as "just per-boot numbering".** The device read `034` while the log's last
   enumeration was `33`. I explained this away as normal re-enumeration. It was not: the ring buffer
   **drops old messages**, so the count of logged enumerations is a *lower bound*, not a total, and the
   missing `034` enumeration was a real event. Numbers that don't reconcile are a signal to go looking,
   never to rationalise.

The general rule this reinforces: **a quiet period only proves recovery if it exceeds the worst gap the
fault produced.** 3 minutes of quiet says nothing when the fault itself managed 3.4 minutes.


## 13. Ownership: the device belonged to another account

> **Status: resolved.** The device is now owned by `tonybuckingham@proton.me`. The diagnosis below is
> kept in the tense it was observed in, because the general case — a second-hand device that already
> has an owner — is the interesting part.

The device was on Device OS 2.3.1, stable and connected to the cloud — but could not be added to the
account because **it already belonged to a different Particle account**.

### Resolution — verified

Ownership was transferred by the existing owner, and the claim then succeeded. Because `Successfully
claimed device` is also printed for a device you *already* own, that line on its own proves nothing —
so the check that actually discriminates is `particle list`, which had returned `No devices found`
throughout and now returns the device. The Cloud API confirms the attribute that matters:

```text
$ particle list
Button [1f0034000e47343432313031] (Photon) is online
  Functions:
    int digitalread (String args)
    int digitalwrite (String args)
    int analogread (String args)
    int analogwrite (String args)

$ GET /v1/devices/1f0034000e47343432313031
  owner: tonybuckingham@proton.me
  online: True
  system_firmware_version: 2.3.1
  status: normal
  serial_number: BTTN-150809-ENGW-0
  functions: [digitalread, digitalwrite, analogread, analogwrite]
```

Three things worth reading off that:

- **`owner` is the field that matters**, and it now names the right account. `particle list` is a
  reflection of it, not an independent confirmation.
- **`serial_number: BTTN-150809-ENGW-0`** — the `BTTN` prefix confirms this is a genuine Particle
  Internet Button kit, consistent with §1.
- **The functions are Tinker's** (`digitalread`/`digitalwrite`/`analogread`/`analogwrite`), so the
  device is running **bare Tinker**, not the InternetButton library. That is expected from the restore
  in §11, and it means the shield is *still* unverified (§8) — Tinker cannot drive WS2812B LEDs, which
  need NeoPixel bit-banged timing rather than a plain `digitalWrite`.

### Evidence, as originally seen

```text
That device belongs to someone else. Would you like to request a transfer?
Failed to claim device: That belongs to someone else.
To request a transfer add ?request_transfer=true to the URL.
```

The logged-in account at the time was `tonybuckingham@proton.me` (`particle whoami`), and `particle
device add --help` shows **no transfer option** — the command takes only `<deviceID>`. So the CLI is
pointing at the Cloud API rather than offering a flag of its own.

### The CLI's hint is written for a different verb

"Add `?request_transfer=true` to the URL" is misleading. The Cloud API reference documents the transfer
as a **`POST` with a body parameter**, which is not a query-string edit:

```text
POST /v1/devices
  id               REQUIRED   device id
  request_transfer REQUIRED   true — requesting a transfer from another user
→ { transfer_id: ... }
```

So the documented form is `curl https://api.particle.io/v1/devices -d id=<id> -d
request_transfer=true`. Worth noting for anyone who follows the CLI's wording literally and wonders why
a query parameter does nothing useful.

### Why `particle list` misled §11

`particle list` prints devices **in your account only** — it returned `No devices found`, which was read
as "the device is unclaimed by anybody". It only means "not claimed by *me*". A device owned by another
account is invisible there but very much owned. The claim attempt is what surfaced the truth.

### Three ways out, and why one was chosen

| Route | Requirement | Notes |
| --- | --- | --- |
| Unclaim from the owning account | access to that account | **Chosen.** Instant, no approval step |
| Request a transfer | the current owner must **approve** it | Leaves the device unusable until they do |
| Particle support | proof of purchase / ownership | Fallback if the old account is unrecoverable |

The owner recognised the device as belonging to **an older Particle account of their own**, which makes
the first row both available and strictly better: a transfer request would mean asking themselves for
permission, and would not take effect until the other account approved.

Note the trap in row two — **a transfer request does not transfer anything by itself.** It returns a
`transfer_id` and waits for the other side. If the old account is unrecoverable, neither of the first
two routes works and it is a support case.

### The mechanism for switching accounts without losing the current login

Rather than logging out, the CLI's **profile** system keeps each account in its own file. Verified by
reading the CLI's own implementation, not by assumption:

- Config file path is `~/.particle/<profile>.config.json`.
- `~/.particle/profile.json` holds the active profile name; the profile `switchProfile()` writes **only
  that file**.
- With **no** `name` key set, the CLI defaults to the profile named **`particle`** — which is this
  machine's current state (`profile.json` contains only `last_version_check`), matching
  `particle profile --list` → `1.) particle`.

Consequence: switching to a second profile writes `profile.json` and creates
`<newprofile>.config.json`, leaving `particle.config.json` — the existing login — **untouched on disk**.
Switching back restores it with no re-login.

### The sequence

Run these **in your own terminal**, not through an assistant: `particle login` prompts for a password.

```sh
export PATH="$HOME/bin:$PATH"

# Keep the current login in its own profile, then authenticate as the OLD account
particle profile old-account
particle login                # type the OLD account's email + password (and MFA if enabled)
particle whoami               # confirm you are the old account

# This is the step that actually frees the device
particle device remove --yes 1f0034000e47343432313031

# Back to the main account, then claim
particle profile particle
particle whoami               # must print tonybuckingham@proton.me
particle device add 1f0034000e47343432313031
```

`particle device remove` is described by the CLI itself as *"Release a device from your account so that
another user may claim it"* — it needs no approval from anyone, which is exactly why this route is
preferred over a transfer.

If `particle profile old-account` does not create the profile, the fallback is simply to log in as the
old account, unclaim, then log in as the main account again. The cost is one extra login, and nothing
on disk is destroyed either way.

### What claiming actually buys

The device **works without being claimed** — it connects to the cloud, as it must for the very first
setup and for the Safe Mode healer of §11. That is why the fault in §12 could be diagnosed, the version
read and written, and Wi-Fi provisioned, all while the device belonged to nobody in particular. What
ownership adds is everything that needs an account to point at:

| Without ownership | With ownership |
| --- | --- |
| No Console listing, no rename or notes | Console management |
| No OTA flash from Console or CLI | `particle flash <device> …` over the air |
| Functions/variables not callable by your account | `particle call` / `particle get` |
| Not usable in the Web IDE | Web IDE flashing |

That last column is now available, and it makes the remaining shield check (§8) easier than the plan
allowed for: the device is **online**, so the library example can be flashed **over the air** rather
than over USB. The `--target 2.3.1` rule still applies — and matters more now, because a binary built
for a newer Device OS is exactly what triggered §11's Safe Mode healer in the first place.



