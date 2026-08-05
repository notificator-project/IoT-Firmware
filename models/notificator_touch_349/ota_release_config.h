#pragma once

/** Public metadata used to authenticate official Touch firmware releases. */
static const char NOTIFICATOR_OTA_MANIFEST_URL[] =
	"https://wpnotif.notificator-project.com/firmware/manifest.json";
static const char NOTIFICATOR_OTA_DEVICE_TYPE[] = "notificator_touch_349";
static const char NOTIFICATOR_OTA_BOARD[] = "waveshare-esp32-s3-touch-lcd-3.49";
static const char NOTIFICATOR_OTA_DEFAULT_CHANNEL[] = "preview";
static const char NOTIFICATOR_OTA_KEY_ID[] =
	"sha256:9eb7eb4a95ad4be2f53d1039036dc68942c74fb00d807c1d81d4c35d04b8be50";

static const char NOTIFICATOR_OTA_PUBLIC_KEY_PEM[] = R"PEM(
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAETfjcX6PYc0Pcgv7Oa69jcO30f2xy
6gNu4vycZbUPYoZRnDrNHqxogRC8oVsZON+ZjZyEl3XsPwxCgMcguoaeVA==
-----END PUBLIC KEY-----
)PEM";
