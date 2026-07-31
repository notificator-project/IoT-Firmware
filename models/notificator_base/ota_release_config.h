#pragma once

/**
 * Official Notificator firmware release configuration.
 *
 * The manifest URL and public key are intentionally part of the firmware and
 * are safe to publish. The matching private signing key must remain outside
 * source control and is used only by the release workflow.
 */
static const char NOTIFICATOR_OTA_MANIFEST_URL[] =
	"https://wpnotif.notificator-project.com/firmware/manifest.json";

static const char NOTIFICATOR_OTA_DEVICE_TYPE[] = "notificator_base";
static const char NOTIFICATOR_OTA_BOARD[] = "esp32c3-supermini-oled";
static const char NOTIFICATOR_OTA_DEFAULT_CHANNEL[] = "stable";
static const char NOTIFICATOR_OTA_KEY_ID[] =
	"sha256:9eb7eb4a95ad4be2f53d1039036dc68942c74fb00d807c1d81d4c35d04b8be50";

static const char NOTIFICATOR_OTA_PUBLIC_KEY_PEM[] = R"PEM(
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAETfjcX6PYc0Pcgv7Oa69jcO30f2xy
6gNu4vycZbUPYoZRnDrNHqxogRC8oVsZON+ZjZyEl3XsPwxCgMcguoaeVA==
-----END PUBLIC KEY-----
)PEM";
