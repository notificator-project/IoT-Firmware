# Notificator Touch 3.49 Diagnostic

This temporary hardware-validation target is for the Waveshare
`ESP32-S3-Touch-LCD-3.49` only. It validates the hardware boundary before the
production Notificator Touch interface and networking runtime are introduced.

The diagnostic checks:

- AXS15231B QSPI display output
- AXS15231B capacitive touch input
- LCD backlight control
- 16 MB flash detection
- 8 MB PSRAM detection and read/write integrity
- RTC and IMU presence on the system I2C bus
- QMI8658-assisted 180-degree landscape rotation
- Touch-controller presence on its dedicated I2C bus
- ES8311 speaker playback through the board's I2S audio path
- BOOT and PWR side-button input, including debounced press counters
- Passive Wi-Fi network scanning

The first screen shows the detected flash, PSRAM, and Wi-Fi results. A tap leaves
a cyan marker at the physical coordinate. Swipe left to open the interaction
screen, tap **Play test chime** to verify the speaker, and swipe left again for
the physical-button screen. That final page shows the live state and press count
for the BOOT and PWR buttons. The RESET button restarts the diagnostic by design.
Swipe right to move back through the pages.

Holding PWR for 1.8 seconds releases TCA9554 output 6, matching Waveshare's
battery shutdown circuit. USB supplies power independently, so the board may
remain on during a tethered test even though the battery latch was released.
RESET is connected directly to the ESP32-S3 enable line and cannot display a
software confirmation before the restart; the diagnostic shows this as a red
warning instead.
Touch coordinates, gestures, and audio results are also printed over USB serial
at 115200 baud.

For the rotation test, hold the display upright until the diagnostic calibrates,
then turn it upside down. The layout and touch mapping rotate together after the
new position remains stable for roughly 220 ms. A short card-flip animation
makes the orientation change visible. A flat display cannot determine
which short edge is up from gravity alone, so rotation pauses in that position.

## Build settings

- Board: ESP32S3 Dev Module
- CPU: 240 MHz
- Flash: 16 MB, QIO 80 MHz
- PSRAM: OPI
- USB mode: Hardware CDC and JTAG
- USB CDC on boot: Enabled
- Partition scheme: 16 MB Flash (2 MB APP / 12.5 MB FATFS)

The partition choice matches the board's factory dual-OTA layout. This target
is intentionally not included in the public installer or release workflow.

Display and touch pin definitions are taken from Waveshare's official example:
https://github.com/waveshareteam/ESP32-S3-Touch-LCD-3.49

The vendored display, touch, codec, and I/O-expander sources retain their
upstream Apache-2.0 or Espressif MIT license notices.
