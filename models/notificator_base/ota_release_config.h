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
	"sha256:d015c69ecc6f1830eac13198232df9a0ca54032ae5bab25842b91c0950485dcb";

static const char NOTIFICATOR_OTA_PUBLIC_KEY_PEM[] = R"PEM(
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEXIC8/LJ2F3wRHY8I81NKStGHMiUd
guQGUShJJZvJxgxjSOdYcvSkE/LbA2mMOrf5xTEUDvZn210NsES+4pmUGw==
-----END PUBLIC KEY-----
)PEM";
