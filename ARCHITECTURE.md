# Firmware Architecture

This document explains the runtime boundaries and the reasons behind the less obvious implementation choices.

## Hardware Budget

The target is an ESP32-C3 SuperMini with 4 MB flash, an SSD1306 128×64 OLED, and a TTP223 digital capacitive sensor.

- Application binary: approximately 1.33 MB with ESP32 Arduino core 3.3.8
- Static/global RAM: approximately 41 KB
- OTA layout: two `0x1E0000` application slots
- Filesystem: none
- Message history: ten compact records in NVS

The firmware avoids external web assets, a filesystem, and large display frame resources. WiFiManager serves the setup portal directly from program flash.

## Source Layout

- `models/notificator_base/notificator_base.ino`: Notificator Base runtime state, OLED rendering, Wi-Fi recovery, MQTT delivery, message history, commands, and the Arduino `setup()`/`loop()`
- `models/notificator_base/ota_release_config.h`: official manifest endpoint, model identity, and public verification key
- `models/notificator_base/ota_security.h` and `models/notificator_base/ota_security.cpp`: strict version parsing, canonical release payloads, and ECDSA signature verification
- `models/notificator_base/portal_ui.h`: self-contained CSS for the captive setup portal
- `models/notificator_base/partitions.csv`: 4 MB dual-OTA flash layout
- `hardware/notificator_base/`: wiring and enclosure reference assets

Each firmware target uses a stable machine-readable ID. The current target is
`notificator_base`; the planned Matter target is `notificator_matter`. A model
gets an independent sketch and OTA release entry rather than compile-time
branches inside another model's firmware.

The OLED and network state machines still share timing and message-history state, so they remain together for this hardware-testing phase. After the touch and screen behavior is validated on physical devices, they can be separated behind narrow interfaces without changing behavior.

## Runtime State Machine

The main loop processes work in this order:

1. Sample and debounce capacitive input.
2. Flush delayed message-history writes.
3. Refresh time and weather data when Wi-Fi is available.
4. Service the non-blocking setup portal when active.
5. Recover Wi-Fi using bounded retries and an eventual setup fallback.
6. Maintain the MQTT connection and subscriptions.
7. Draw one UI state: hold feedback, device information, gesture feedback, idle screen, or notification.

Early returns are intentional. They prevent setup, recovery, OTA feedback, and the normal notification renderer from drawing over one another.

## Local Configuration

WiFiManager owns Wi-Fi credentials. Notificator stores the following values in the `wpnotif` NVS namespace:

| NVS key | Purpose |
| --- | --- |
| `mqtt_host` | HiveMQ cluster hostname |
| `mqtt_port` | Secure MQTT listener port |
| `mqtt_user` | MQTT access username |
| `mqtt_pass` | MQTT access password |
| `mqtt_topic` | Root topic prefix |

Blank MQTT-password submissions retain the saved secret. Saved secrets are not inserted back into the HTML form. OTA trust configuration is compiled into each model and is not editable in the captive portal.

## MQTT Contract

Notificator does not operate a default MQTT broker. The device connects directly
to the HiveMQ Cloud cluster configured by its owner. HiveMQ Cloud is the only
provider supported by the current release. WordPress must use the same cluster
and topic prefix.

For topic prefix `notificator-project` and device ID `abc123`:

| Topic | Direction | Purpose |
| --- | --- | --- |
| `notificator-project/abc123/messages` | Subscribe | Notification payloads |
| `notificator-project/abc123/cmd` | Subscribe | Device commands, including channel-based OTA checks |
| `notificator-project/abc123/status` | Publish, retained | Online, firmware, health, and OTA results |

The device ID is derived from the ESP32 eFuse MAC and is displayed in the information screen.

### Notification Payloads

The message topic accepts a structured JSON object:

```json
{
  "title": "New order",
  "body": "Order #1042 requires attention.",
  "type": "generic_notification",
  "severity": "info"
}
```

`title` and `body` are displayed. `severity` takes precedence over `type` when
the compact header badge is generated. Unrecognized values are truncated to six
characters.

For compatibility, a plain-text payload is also accepted:

```text
Title|Message body|severity
```

A payload without separators is treated as the message body. Stored headers and
payloads are intentionally truncated to the limits in the packed history record.

### Command Payloads

Commands are JSON objects published to the device command topic:

| Command | Required fields | Effect |
| --- | --- | --- |
| `idle_theme` | `value` (`0`, `1`, or `2`) | Select clock, hybrid, or weather idle display |
| `clear_msgs` | None | Delete runtime and saved notification history |
| `mark_all_read` | None | Mark every stored notification read |
| `weather_config` | Optional coordinate, city, and timezone fields | Override the automatically detected weather location |
| `ota` | Optional `channel` (`stable` or `preview`) and `force` | Check and install an authenticated model-specific release |

Example:

```json
{
  "cmd": "idle_theme",
  "value": 1
}
```

History deletion is not available through the capacitive control. It requires
an MQTT command on the device-specific command topic.

### Retained Status Payload

The status topic receives a retained JSON object so a controller can inspect
the most recently published device state:

```json
{
  "type": "device_status",
  "event": "online",
  "deviceId": "abc123",
  "firmware": "1.1.0",
  "uptime": 120,
  "freeHeap": 286044,
  "rssi": -58,
  "status": "ready"
}
```

OTA lifecycle messages can additionally contain `targetVersion` and `error`.
The exact heap and signal values above are illustrative.

## External Network Requests

The normal notification path uses the user-configured HiveMQ broker. Weather
idle themes additionally use:

| Service | When used | Data sent |
| --- | --- | --- |
| `ip-api.com` | Approximate location refresh when no manual location exists | Source public IP, which is inherent to the request |
| `api.open-meteo.com` | Current weather refresh | Latitude, longitude, and timezone |
| NTP pool servers | Clock synchronization | Standard NTP request |
| `wpnotif.notificator-project.com` | When checking for a firmware release | Model, channel, and normal HTTPS request metadata |
| Signed firmware URL | Only when a newer authenticated release is available | Firmware download request |

No MQTT password, private signing key, or notification content is sent to the
weather or firmware services. A private firmware-signing key never exists on a
device. A manual weather location prevents IP-based location lookups.

The current Open-Meteo client uses encrypted transport without certificate
verification to avoid embedding another CA chain in the constrained firmware.
Forecast data is therefore not treated as trusted control input. MQTT and OTA
use certificate verification and carry the security-sensitive traffic.

## Touch Interaction

The TTP223 idle level is sampled during boot, which supports common active-high and active-low module configurations. Input is debounced before gesture timing starts.

| Gesture | Action |
| --- | --- |
| Tap | Wake or next notification |
| Double tap | Toggle current notification read/unread |
| Triple tap | Open device and connection information |
| Hold 1.8 seconds | Mark all notifications read |
| Hold 6 seconds | Open setup |

The firmware waits 420 ms before resolving a tap sequence. A hold action is applied only after release. This reduces accidental actions caused by capacitive jitter.

## OTA Security

An OTA release is accepted only when all checks pass:

1. The requested channel is `stable` or `preview`.
2. The manifest is downloaded over certificate-verified HTTPS from the fixed official URL.
3. The schema, model identifier, board identifier, semantic version, image size, key identifier, and algorithm are valid.
4. The manifest entry has a valid ECDSA P-256 signature from the model's embedded public key.
5. The target version is newer unless the authenticated command requests `force`.
6. The binary is downloaded over certificate-verified HTTPS.
7. The streamed image size and SHA-256 digest match the signed manifest before the OTA slot is activated.

The canonical release payload is:

```text
NOTIFICATOR-OTA-V1
channel
deviceType
board
version
url
sha256
size
releasedAt
```

The update is streamed into the inactive OTA slot while its digest is
calculated. OLED progress is rate-limited to avoid excessive I2C work, and the
result is published to the retained status topic before restart when possible.

## Flash Migration

Application-only OTA does not rewrite an ESP32 partition table. Devices using the older 1.25 MB slot layout need one complete USB flash that includes `partitions.csv`. This is a one-time migration; subsequent firmware updates can use A/B OTA.

## Hardware Validation Checklist

Before tagging a firmware release:

1. Flash a clean device with the complete image and custom partition table.
2. Complete setup using iOS and Android captive-portal flows.
3. Confirm HiveMQ reconnects after Wi-Fi loss and device restart.
4. Publish plain-text and JSON notifications, including long content.
5. Test every gesture with slow, fast, and noisy touch input.
6. Confirm secrets are not shown when setup is reopened.
7. Send rejected OTA cases: unsupported channel, wrong model or board, signature mismatch, hash mismatch, and older version.
8. Complete a valid OTA and verify the new version boots and publishes ready status.
9. Confirm free heap and OTA space remain healthy after repeated messages and portal sessions.
