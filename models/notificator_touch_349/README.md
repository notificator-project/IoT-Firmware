# Notificator Touch 3.49

`notificator_touch_349` is the production firmware target for the Waveshare
ESP32-S3-Touch-LCD-3.49. It is separate from the hardware diagnostic so factory
and field troubleshooting can continue without changing the user-facing model.

The current public-development firmware is **0.9.2 Preview**. Touch remains a
pre-1.0 model while its interaction design and field-update flow are validated.

## Current development slice

- Branded boot experience and modern 640 × 172 dashboard, alert inbox,
  device-status, and settings pages
- Canonical lowercase pairing ID shown during setup, on the dashboard, and on
  the device page
- Automatic first-run `WPNOTIF-<ID>` captive portal at `192.168.4.1`
- Mobile-friendly Wi-Fi and HiveMQ Cloud configuration stored in ESP32 NVS
- Authenticated TLS MQTT with the user's own HiveMQ Cloud credentials
- Live notification subscription and retained device telemetry
- Calibrated GPIO4 battery-voltage sampling with a smoothed Li-ion percentage
  estimate in the UI and retained telemetry
- Plugin-blue interface with larger labels and persistent Home, Alerts, and
  Device touch targets
- A local six-alert inbox with explicit Newer and Older controls
- Local received times on the latest-alert card and alert-history entries
- Matching Info, Warning, and Critical alert accents in blue, amber, and red
- On-device Wi-Fi scanning, password entry, connection testing, and automatic
  rollback to the previous network when a new connection fails
- A black digital-clock idle screen after 60 seconds, with one-tap wake and
  automatic time-offset detection during setup
- An unread-alert clock cue: the time changes from white to red while any local
  alert remains unread, then returns to white after the alerts are read
- Clock, Weather & Clock, and Weather idle modes configured from the mobile app
- Saved city, coordinates, and POSIX timezone configuration with Open-Meteo
  current conditions
- Temporary uppercase-topic compatibility for device IDs saved by earlier
  Touch development builds
- Four-second BOOT hold to reopen setup without erasing saved credentials
- Horizontal swipe navigation with orientation-aware touch mapping
- QMI8658 automatic 180-degree landscape rotation
- ES8311 notification chime
- BOOT button local alert injection for end-to-end UI testing
- PWR button shutdown after a deliberate 1.8-second hold
- Explicit warning that RESET restarts immediately
- Model-locked signed OTA updates from the Touch preview channel, with manifest
  signature, board identity, image size, and SHA-256 verification
- AXS15231B display, 8 MB PSRAM, 16 MB flash, RTC, and IMU validation

Short-press BOOT to generate local test data. The test notification is not
persisted. Hold BOOT for four seconds to reopen setup. The device never embeds
broker credentials in a public build and never sends them to the Notificator
API.

## First-time setup

1. Power on the display and note the pairing ID shown on screen.
2. Join the `WPNOTIF-<ID>` Wi-Fi network from a phone or computer.
3. Open `http://192.168.4.1` if the setup portal does not appear automatically.
4. Choose the local Wi-Fi network and enter the HiveMQ Cloud cluster hostname,
   secure port, username, and password.
5. Keep the topic prefix as `notificator-project` and save.
6. Add the displayed pairing ID in the Notificator mobile app.

The setup page detects the phone's current UTC offset for the clock. Adjust the
value before saving if the display will live in a different timezone. This is a
fixed offset, so regions that change clocks seasonally should reopen setup when
their UTC offset changes.

The MQTT password field is deliberately blank whenever setup is reopened. A
blank submission keeps the password already stored on the device.

## Everyday controls

- Tap **Home**, **Alerts**, **Device**, or **Settings** in the bottom bar.
- Open **Settings** to change Wi-Fi without another device. Select a nearby
  network, type its password on screen, and connect. The firmware keeps the
  previous credentials until the new network obtains an IP address and restores
  them automatically if the test times out.
- Adjust display brightness and speaker volume from **Settings**. Both values
  are stored on the device and can also be changed from the mobile app.
- Tap an unread alert to mark its local preview as read. Use **Newer** and
  **Older** to browse the six most recent alerts kept in memory.
- Leave the display untouched for 60 seconds to enter the clock screen; any
  touch, notification, or physical button wakes it. Battery percentage stays
  hidden on this ambient view to keep the clock and weather presentation clean.
  The time is red whenever the local inbox contains an unread alert and returns
  to white after every alert has been read.
- Battery percentage is an estimate derived from the board's documented 3:1
  ADC divider. `USB` is shown when a plausible battery voltage is not detected.
- The board does not expose a dependable charger-status signal to the ESP32, so
  the interface deliberately avoids claiming that the battery is charging.
- Hold BOOT for four seconds whenever the phone-based captive portal is needed
  as a recovery alternative to the on-device Wi-Fi wizard.

## Runtime boundaries

1. Persistent alert history across restarts and richer command handling.
2. Touch remains on the preview OTA channel until the model reaches 1.0.

The target will use the same default topic prefix as Notificator Base:
`notificator-project`.

## Build settings

- Board: ESP32S3 Dev Module
- CPU: 240 MHz
- Flash: 16 MB, QIO
- PSRAM: OPI
- USB mode: Hardware CDC and JTAG
- USB CDC on boot: Enabled
- Partition scheme during hardware/UI development: 16 MB Flash

Display, touch, codec, and I/O-expander sources are derived from Waveshare's
official examples and retain their upstream Apache-2.0 or Espressif MIT notices.
