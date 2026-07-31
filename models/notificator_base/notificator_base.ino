#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <mbedtls/sha256.h>

#define MQTT_MAX_PACKET_SIZE 1024
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>
#include <time.h>
#include <Preferences.h>
#include <vector>

#include "ota_security.h"
#include "ota_release_config.h"
#include "portal_ui.h"

/**
 * @file notificator_base.ino
 * @brief Main runtime for the Notificator Base ESP32-C3 OLED device.
 *
 * This translation unit owns the hardware lifecycle and the state that is
 * intentionally shared by the display, capacitive input, Wi-Fi, MQTT, and
 * notification history. Low-coupling OTA validation and captive-portal styling
 * live in separate modules.
 *
 * Target hardware:
 * - ESP32-C3 SuperMini
 * - SSD1306 128x64 I2C OLED
 * - TTP223 capacitive touch sensor
 *
 * Persistent data is stored in the `wpnotif` Preferences namespace. Update
 * FW_VERSION, FW_VERSION_DATE, README.md, and ARCHITECTURE.md together when
 * release behavior changes.
 */
#define FW_NAME "Notificator Base Firmware"
#define FW_VERSION "1.1.1"
#define FW_VERSION_DATE "2026-07-31"

/*
  Notificator Base ESP32-C3 firmware
  --------------------------------
  Responsibilities
  - Input: touch-first gesture handler for the TTP223 capacitive sensor.
  - Transport: MQTT message topic + MQTT command topic per deviceId.
  - UI: status bar + message viewer + idle themes (clock / weather).
  - Storage: ring buffer persisted in Preferences key "hist".

  Runtime message payload in RAM
  - Stored as: "title|body"
  - If separator is missing, content is rendered as a single text block.

  Gesture map
  - 1 tap: wake the display or show the next message
  - 2 taps: toggle the current message read/unread
  - 3 taps: show device and connection information
  - hold >= 1.8s: mark all messages read
  - hold >= 6s: start the setup portal

  Visual unread cues
  - Status bar envelope blinks when any unread message exists.
  - Message header shows explicit READ/UNREAD badge for current message.
*/

// -------------------- HW --------------------
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C

#define I2C_SDA 20
#define I2C_SCL 21

#define TTP223_PIN 0
#define BUTTON_DEBOUNCE_MS 20
#define BUTTON_LONG_PRESS_MS 1800
#define BUTTON_SETUP_LONG_PRESS_MS 6000
#define TAP_WINDOW_MS 420
#define TAP_MIN_PRESS_MS 25

#define SHOW_ID_TAP_COUNT 3
#define SHOW_ID_DISPLAY_MS 4000

#define MESSAGE_BUFFER_SIZE 10

#define WIFI_AP_PREFIX "WPNOTIF-"
#define WIFI_AP_PASSWORD ""

// -------------------- MQTT --------------------
#define MQTT_USE_TLS true

// Optional build-time defaults for pre-provisioned devices. The setup portal
// can replace these values, so public builds do not need broker credentials.
const char *DEFAULT_MQTT_HOST = "";
const uint16_t DEFAULT_MQTT_PORT = 8883;
const char *DEFAULT_MQTT_USERNAME = "";
const char *DEFAULT_MQTT_PASSWORD = "";
const char *DEFAULT_MQTT_TOPIC_PREFIX = "notificator-project";
const char *MQTT_CA_CERT = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

// OTA TLS trust anchor (kept separate from MQTT for independent rotation).
const char *OTA_CA_CERT = MQTT_CA_CERT;

WiFiClientSecure tlsClient;
PubSubClient mqttClient(tlsClient);

unsigned long lastMqttAttemptMs = 0;
static const unsigned long MQTT_RECONNECT_MS = 2000;
String mqttHost;
uint16_t mqttPort = DEFAULT_MQTT_PORT;
String mqttUsername;
String mqttPassword;
String mqttTopicPrefix = DEFAULT_MQTT_TOPIC_PREFIX;
bool mqttConfigSaveRequested = false;
bool mqttConfigValid = false;

/**
 * Authenticated release metadata loaded from the official OTA manifest.
 *
 * The signature covers every field required to select and verify the binary.
 * Release notes remain presentation-only and are not needed by the device.
 */
struct OtaRelease
{
	String channel;
	String deviceType;
	String board;
	String version;
	String url;
	String sha256;
	String releasedAt;
	String signature;
	String keyId;
	size_t size = 0;
};

// -------------------- WiFi state machine --------------------
static const unsigned long WIFI_RETRY_MS = 3500;
static const unsigned long WIFI_DRAW_MS = 600;
static const unsigned long WIFI_POST_CONNECT_GRACE_MS = 4000;
static const unsigned long WIFI_HARD_RESET_AFTER_MS = 90000; // 90s
static const unsigned long WIFI_PORTAL_AFTER_MS = 240000;	 // 4 minutes
static const unsigned long WIFI_STACK_RESET_MS = 250;

unsigned long wifiConnectingSinceMs = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long lastWifiDrawMs = 0;
unsigned long wifiConnectedAtMs = 0;
bool wifiHardResetDone = false;

// -------------------- OLED/LED --------------------
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

unsigned long lastHeartbeatMs = 0;
bool heartbeatOn = false;

unsigned long lastMsgLedFlashMs = 0;
static const unsigned long MSG_LED_FLASH_COOLDOWN_MS = 1200;

// -------------------- Device ID / topics --------------------
String deviceId = "";
String mqttSubTopic = "";
String mqttCmdTopic = "";
String mqttStatusTopic = "";
String apSsid = "";
unsigned long showIdUntilMs = 0;
bool showIdSticky = false;
unsigned long showNoMessagesUntilMs = 0;
static const unsigned long NO_MESSAGES_DISPLAY_MS = 1400;
enum class UiFeedback : uint8_t
{
	None,
	MarkedRead,
	MarkedUnread,
	AllRead
};
UiFeedback uiFeedback = UiFeedback::None;
unsigned long uiFeedbackUntilMs = 0;
static const unsigned long UI_FEEDBACK_MS = 900;

// -------------------- WiFiManager --------------------
/*
 * WiFiManager owns Wi-Fi credentials. The fields below cover Notificator
 * configuration only. Password and signing-key buffers are intentionally
 * cleared before the portal is rendered, so saved secrets never return to the
 * browser.
 */
WiFiManager wm;
bool portalRunning = false;
volatile bool portalClientConnected = false;
bool deviceConfigured = false;

char mqttHostField[97] = "";
char mqttPortField[6] = "8883";
char mqttUsernameField[65] = "";
char mqttPasswordField[97] = "";
char mqttTopicField[97] = "notificator-project";

WiFiManagerParameter mqttSection(
	"<section class=\"notif-group\"><div class=\"notif-section\">"
	"<span class=\"notif-provider\">HiveMQ Cloud only</span>"
	"<strong>Notification delivery</strong>"
	"<span>Create a free Serverless cluster before setup. Use the hostname and device credential from Access Management.</span></div>");
WiFiManagerParameter mqttHostParameter(
	"mqtt_host",
	"HiveMQ cluster hostname",
	mqttHostField,
	sizeof(mqttHostField),
	"required maxlength=\"96\" autocapitalize=\"none\" autocomplete=\"off\" placeholder=\"abc123.s1.eu.hivemq.cloud\"");
WiFiManagerParameter mqttPortParameter(
	"mqtt_port",
	"Secure MQTT port",
	mqttPortField,
	sizeof(mqttPortField),
	"required type=\"number\" min=\"1\" max=\"65535\" inputmode=\"numeric\"");
WiFiManagerParameter mqttUsernameParameter(
	"mqtt_user",
	"MQTT username",
	mqttUsernameField,
	sizeof(mqttUsernameField),
	"required maxlength=\"64\" autocapitalize=\"none\" autocomplete=\"username\"");
WiFiManagerParameter mqttPasswordParameter(
	"mqtt_pass",
	"MQTT password",
	mqttPasswordField,
	sizeof(mqttPasswordField),
	"maxlength=\"96\" type=\"password\" autocomplete=\"new-password\" placeholder=\"Leave blank to keep the saved password\"");
WiFiManagerParameter mqttTopicParameter(
	"mqtt_topic",
	"Topic prefix",
	mqttTopicField,
	sizeof(mqttTopicField),
	"required maxlength=\"96\" autocapitalize=\"none\" autocomplete=\"off\" placeholder=\"notificator-project\"");
WiFiManagerParameter mqttSectionEnd("</section>");

unsigned long animLastMs = 0;
uint8_t animFrame = 0;

unsigned long portalStartMs = 0;
bool portalStartChecked = false;
bool setupScreenDrawn = false;

// -------------------- Preferences --------------------
Preferences prefs;

// -------------------- Remote / UI state --------------------
// 0 = clock (default), 1 = weather+clock hybrid, 2 = weather
uint8_t idleTheme = 0;

// prevent idle UI jumps right after actions/messages
static const unsigned long NO_IDLE_AFTER_ACTION_MS = 2000;
unsigned long noIdleUntilMs = 0;

// -------------------- Geo by IP (for weather theme 2) --------------------
// Weather follows the device's public IP (the network the ESP32 is on).
static const unsigned long GEO_REFRESH_MS = 12UL * 60UL * 60UL * 1000UL; // 12 hours
unsigned long lastGeoFetchMs = 0;
bool geoHasData = false;
bool geoFetching = false;
bool geoManualOverride = false;

float geoLat = 37.9838f; // fallback Athens
float geoLon = 23.7275f; // fallback Athens
String geoCity = "ATH";
String geoTz = "Europe/Athens";

// -------------------- Weather (theme 2) --------------------
static const unsigned long WEATHER_REFRESH_MS = 15UL * 60UL * 1000UL; // 15 min
unsigned long lastWeatherFetchMs = 0;
bool weatherHasData = false;
bool weatherFetching = false;

float weatherTempC = 0;
float weatherWindKmh = 0;
uint8_t weatherCode = 255;

// -------------------- Persistent history --------------------
/*
 * The in-memory ring uses String for convenient rendering. The packed structs
 * define the stable NVS wire format and bound flash writes to predictable
 * sizes. HISTORY_VERSION must change whenever this binary layout changes.
 */
static const uint16_t MAX_TOPIC_CHARS = 24;
static const uint16_t MAX_PAYLOAD_CHARS = 90;

struct __attribute__((packed)) PersistMsg
{
	char topic[MAX_TOPIC_CHARS];
	char payload[MAX_PAYLOAD_CHARS];
	uint8_t read;
};

struct __attribute__((packed)) PersistHistory
{
	uint32_t magic;
	uint8_t version;
	uint8_t count;
	uint8_t head;
	uint8_t current;
	PersistMsg msgs[MESSAGE_BUFFER_SIZE];
};

static const uint32_t HISTORY_MAGIC = 0x57504E46; // 'WPNF'
static const uint8_t HISTORY_VERSION = 3;

/*
 * Ring-buffer invariants:
 * - messageHead points to the oldest valid entry.
 * - messageCount never exceeds MESSAGE_BUFFER_SIZE.
 * - currentIndex is a physical array index, not an offset from messageHead.
 * - a new message becomes current and starts unread.
 */
struct MqttMessage
{
	String topic;
	String payload;
	bool read;
};

MqttMessage messageBuffer[MESSAGE_BUFFER_SIZE];
size_t messageCount = 0;
size_t messageHead = 0;
size_t currentIndex = 0;
bool historyActive = false;

bool historyDirty = false;
unsigned long historyDirtySinceMs = 0;
static const unsigned long HISTORY_SAVE_DELAY_MS = 1200;

// -------------------- Message flip --------------------
// When payload contains both title and body, alternate display phases.
static const unsigned long MESSAGE_TITLE_MS = 3200;
static const unsigned long MESSAGE_BODY_MS = 2600;
bool showTitlePhase = true;
unsigned long lastFlipMs = 0;
size_t flipIndex = (size_t)-1;

// -------------------- Unread blink --------------------
unsigned long lastUnreadBlinkMs = 0;
bool unreadBlinkOn = false;
static const unsigned long RECEIVING_BADGE_MS = 1200;
unsigned long receivingUntilMs = 0;

// -------------------- Time/NTP --------------------
bool ntpStarted = false;
bool placeholdersRestamped = false;

// -------------------- RSSI smoothing --------------------
int16_t smoothedRssi = -999;
static const float RSSI_ALPHA = 0.35f;
static const unsigned long RSSI_UPDATE_MS = 700;
unsigned long lastRssiUpdateMs = 0;

// -------------------- Capacitive touch state --------------------
static bool ttpStable = false;
static bool ttpRawLast = false;
static unsigned long ttpRawChangedMs = 0;

static bool touchDown = false;
static unsigned long touchDownMs = 0;
static uint8_t ttpIdleLevel = LOW;
static bool holdPreviewActive = false;
static unsigned long holdPreviewMs = 0;
static const unsigned long HOLD_PREVIEW_START_MS = BUTTON_LONG_PRESS_MS;

static uint8_t tapCount = 0;
static bool tapPending = false;
static unsigned long tapDeadlineMs = 0;
static bool tapSequenceStartedFromIdle = false;

// -------------------- Idle --------------------
static const unsigned long IDLE_AFTER_MS = 45000;
unsigned long lastUserOrMsgMs = 0;

static const unsigned long CLOCK_REDRAW_MS = 1000;
unsigned long lastClockDrawMs = 0;
static const unsigned long IDLE_HYBRID_PHASE_MS = 2500;

// -------------------- Forward decls --------------------
void drawStatusBar(bool dim = false);
void drawWrappedMessage(const String &topic, const String &message, bool unreadMark);

bool hasMessages();
uint16_t unreadCount();

void markHistoryDirty();
void maybeSaveHistory();
void saveHistoryToPrefsNow();
void loadHistoryFromPrefs();
void clearHistoryInRam();
void wipeHistoryPrefs();

bool findFirstUnreadIndex(size_t &outIdx);
void focusFirstUnreadIfAny();

void showMessageAt(size_t idx, bool markRead);
void showCurrentMessage(bool markRead);
void gotoPrevMessage(bool markRead);
void markCurrentReadAndPersist();
void toggleCurrentReadStateAndPersist();
void markAllReadAndPersist();
void clearAllMessagesAndShowFeedback();

void startSetupPortal();
void finalizeSetupAfterPortal();
void setupMqttClient();
void connectToMqtt();
void loadMqttConfig();
bool saveMqttConfigFromPortal();
bool isMqttConfigComplete();
void rebuildMqttTopics();
void configureSetupPortal();
void syncMqttPortalParameters();

void drawBootWelcomeScreen();
void drawSetupInstructions();
void drawPortalAnimationFrame();
void drawDeviceIdScreen();
void showNoMessagesOverlay();
void drawNoMessagesScreen();
void drawGestureFeedback();
void updateLedIndicator();
void drawHoldCounter(unsigned long heldMs);

bool timeReady();
String humanNow();
void restampTimePlaceholdersIfReady();
void applyDeviceTimezone();
const char *resolveTimezonePosix();

void startStaConnectStable();
void hardResetWiFiStackOnce();

int16_t readRssiRaw();
void updateSmoothedRssi();
uint8_t rssiBars();
void drawRssiBars(uint8_t bars, bool dim);

void handleTouchInput();
void resetMessageFlipState(size_t idx);
void resetTapSequence();

// idle
// clock + weather idle themes
void drawIdleClockFrame();
void drawIdleHybridFrame();
String humanTimeHHMM();

// geo + weather
void maybeFetchGeoByIP();
bool fetchGeoByIPNow();
void maybeFetchWeather();
bool fetchWeatherNow();
void drawIdleWeatherFrame();
const char *weatherCodeToShort(uint8_t code);

// commands
void loadIdleTheme();
void saveIdleTheme(uint8_t v);
void loadWeatherConfig();
void saveWeatherConfig(bool manual);
void handleCmdJson(const String &json);
bool fetchOfficialOtaRelease(const String &channel, OtaRelease &release, String &error);
void performOfficialOtaUpdate(const String &channel, bool force);
bool streamVerifiedOtaImage(const OtaRelease &release, String &error);
bool publishDeviceStatus(const char *eventName, const char *status = nullptr, const String &targetVersion = "", const String &error = "");

static inline void bumpNoIdleGuard()
{
	unsigned long now = millis();
	noIdleUntilMs = now + NO_IDLE_AFTER_ACTION_MS;
	lastUserOrMsgMs = now;
}

void resetTapSequence()
{
	tapCount = 0;
	tapPending = false;
	tapDeadlineMs = 0;
	tapSequenceStartedFromIdle = false;
}

// -------------------- Helpers --------------------
static void safeCopyToC(char *dst, size_t dstSize, const String &src)
{
	if (!dst || dstSize == 0)
		return;
	size_t n = src.length();
	if (n >= dstSize)
		n = dstSize - 1;
	memcpy(dst, src.c_str(), n);
	dst[n] = '\0';
}

void drawStatus(const char *title, const char *line)
{
	display.clearDisplay();
	display.setTextSize(1);
	display.setTextColor(SSD1306_WHITE);
	display.setCursor(0, 0);
	display.println(title);
	if (line && line[0])
	{
		display.setCursor(0, 16);
		display.println(line);
	}
	display.display();
}

void drawCenteredText(const char *line1, const char *line2 = nullptr, uint8_t size = 2)
{
	display.clearDisplay();
	display.setTextColor(SSD1306_WHITE);

	auto centerX = [&](const char *s, uint8_t ts) -> int
	{
		int16_t x1, y1;
		uint16_t w, h;
		display.setTextSize(ts);
		display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
		return (OLED_WIDTH - (int)w) / 2;
	};

	if (line2 && line2[0])
	{
		int yTop = 18;
		display.setTextSize(size);
		display.setCursor(centerX(line1, size), yTop);
		display.println(line1);
		display.setCursor(centerX(line2, size), yTop + (size * 12));
		display.println(line2);
	}
	else
	{
		int y = 28;
		display.setTextSize(size);
		display.setCursor(centerX(line1, size), y);
		display.println(line1);
	}

	display.display();
}

void setOnboardLed(bool on)
{
	(void)on;
}

void setOnboardLedColor(uint8_t r, uint8_t g, uint8_t b)
{
	(void)r;
	(void)g;
	(void)b;
}

void flashOnboardLedColor(uint8_t r, uint8_t g, uint8_t b, uint16_t durationMs = 60)
{
	(void)r;
	(void)g;
	(void)b;
	(void)durationMs;
}

void flashOnboardLed(uint16_t durationMs = 60)
{
	flashOnboardLedColor(255, 255, 255, durationMs);
}

void updateLedIndicator()
{
	// LED disabled to reduce firmware size.
}

void loadConfiguredFlag()
{
	prefs.begin("wpnotif", true);
	deviceConfigured = prefs.getBool("configured", false);
	prefs.end();
}

void saveConfiguredFlag(bool v)
{
	prefs.begin("wpnotif", false);
	prefs.putBool("configured", v);
	prefs.end();
}

void saveLastSsidForInfo(const String &ssid)
{
	if (!ssid.length())
		return;
	prefs.begin("wpnotif", false);
	prefs.putString("lastSsid", ssid);
	prefs.end();
}

static String normalizeMqttTopicPrefix(String value)
{
	value.trim();
	while (value.startsWith("/"))
		value.remove(0, 1);
	while (value.endsWith("/"))
		value.remove(value.length() - 1);
	return value;
}

/**
 * Validate a broker hostname entered through the local portal.
 *
 * The value must be a bare hostname. Schemes, paths, and whitespace are
 * rejected because PubSubClient receives the port separately.
 */
static bool isValidMqttHost(String value)
{
	value.trim();
	if (!value.length() || value.length() > 96)
		return false;
	if (value.indexOf("://") >= 0 || value.indexOf('/') >= 0 || value.indexOf(' ') >= 0)
		return false;
	return true;
}

/** Reject MQTT wildcards and whitespace in the user-controlled topic root. */
static bool isValidMqttTopicPrefix(const String &value)
{
	if (!value.length() || value.length() > 96)
		return false;
	return value.indexOf('#') < 0 && value.indexOf('+') < 0 && value.indexOf(' ') < 0;
}

/** Return whether the locally stored broker configuration can be used. */
bool isMqttConfigComplete()
{
	return isValidMqttHost(mqttHost) &&
		   mqttPort > 0 &&
		   mqttUsername.length() > 0 &&
		   mqttPassword.length() > 0 &&
		   isValidMqttTopicPrefix(mqttTopicPrefix);
}

/** Rebuild all per-device topics after identity or prefix changes. */
void rebuildMqttTopics()
{
	const String root = normalizeMqttTopicPrefix(mqttTopicPrefix);
	mqttSubTopic = root + "/" + deviceId + "/messages";
	mqttCmdTopic = root + "/" + deviceId + "/cmd";
	mqttStatusTopic = root + "/" + deviceId + "/status";
}

/**
 * Load broker values from the `wpnotif` NVS namespace.
 *
 * Configuration is normalized and validated before mqttConfigValid is set.
 */
void loadMqttConfig()
{
	prefs.begin("wpnotif", true);
	mqttHost = prefs.getString("mqtt_host", DEFAULT_MQTT_HOST);
	mqttPort = prefs.getUShort("mqtt_port", DEFAULT_MQTT_PORT);
	mqttUsername = prefs.getString("mqtt_user", DEFAULT_MQTT_USERNAME);
	mqttPassword = prefs.getString("mqtt_pass", DEFAULT_MQTT_PASSWORD);
	mqttTopicPrefix = prefs.getString("mqtt_topic", DEFAULT_MQTT_TOPIC_PREFIX);
	prefs.end();

	mqttHost.trim();
	mqttUsername.trim();
	mqttTopicPrefix = normalizeMqttTopicPrefix(mqttTopicPrefix);
	mqttConfigValid = isMqttConfigComplete();
}

/**
 * Populate non-secret setup fields and blank the broker password input.
 *
 * A blank password or signing key means "retain the saved value" when the
 * portal is submitted.
 */
void syncMqttPortalParameters()
{
	safeCopyToC(mqttHostField, sizeof(mqttHostField), mqttHost);
	snprintf(mqttPortField, sizeof(mqttPortField), "%u", mqttPort);
	safeCopyToC(mqttUsernameField, sizeof(mqttUsernameField), mqttUsername);
	mqttPasswordField[0] = '\0';
	safeCopyToC(mqttTopicField, sizeof(mqttTopicField), mqttTopicPrefix);
	mqttHostParameter.setValue(mqttHostField, sizeof(mqttHostField));
	mqttPortParameter.setValue(mqttPortField, sizeof(mqttPortField));
	mqttUsernameParameter.setValue(mqttUsernameField, sizeof(mqttUsernameField));
	mqttPasswordParameter.setValue("", sizeof(mqttPasswordField));
	mqttTopicParameter.setValue(mqttTopicField, sizeof(mqttTopicField));
}

/**
 * Validate and persist Notificator fields submitted by WiFiManager.
 *
 * @return true when the resulting broker configuration is complete and safe
 *         to use; false without replacing the stored values otherwise.
 */
bool saveMqttConfigFromPortal()
{
	String newHost = mqttHostParameter.getValue();
	String newUsername = mqttUsernameParameter.getValue();
	String newPassword = mqttPasswordParameter.getValue();
	String newTopicPrefix = normalizeMqttTopicPrefix(String(mqttTopicParameter.getValue()));
	String portText = mqttPortParameter.getValue();

	newHost.trim();
	newUsername.trim();
	newPassword.trim();
	portText.trim();
	const long parsedPort = portText.toInt();
	if (!isValidMqttHost(newHost) ||
		parsedPort < 1 || parsedPort > 65535 ||
		!newUsername.length() ||
		(!newPassword.length() && !mqttPassword.length()) ||
		!isValidMqttTopicPrefix(newTopicPrefix))
	{
		mqttConfigValid = false;
		return false;
	}

	mqttHost = newHost;
	mqttPort = (uint16_t)parsedPort;
	mqttUsername = newUsername;
	if (newPassword.length())
		mqttPassword = newPassword;
	mqttTopicPrefix = newTopicPrefix;

	prefs.begin("wpnotif", false);
	prefs.putString("mqtt_host", mqttHost);
	prefs.putUShort("mqtt_port", mqttPort);
	prefs.putString("mqtt_user", mqttUsername);
	prefs.putString("mqtt_pass", mqttPassword);
	prefs.putString("mqtt_topic", mqttTopicPrefix);
	prefs.end();

	mqttConfigValid = true;
	rebuildMqttTopics();
	return true;
}

/** Register the custom Notificator fields and offline-safe portal styling. */
void configureSetupPortal()
{
	wm.setTitle("Notificator setup");
	wm.setDarkMode(false);
	wm.setCustomHeadElement(NOTIFICATOR_PORTAL_HEAD);
	wm.setCustomMenuHTML(NOTIFICATOR_PORTAL_MENU);
	wm.setShowStaticFields(true);
	wm.setShowDnsFields(false);

	wm.addParameter(&mqttSection);
	wm.addParameter(&mqttHostParameter);
	wm.addParameter(&mqttPortParameter);
	wm.addParameter(&mqttUsernameParameter);
	wm.addParameter(&mqttPasswordParameter);
	wm.addParameter(&mqttTopicParameter);
	wm.addParameter(&mqttSectionEnd);

	wm.setSaveParamsCallback([]()
							 { mqttConfigSaveRequested = true; });
}

// -------------------- Idle theme persistence --------------------
void loadIdleTheme()
{
	prefs.begin("wpnotif", true);
	idleTheme = (uint8_t)prefs.getUChar("idleTheme", 0);
	prefs.end();
	if (!(idleTheme == 0 || idleTheme == 1 || idleTheme == 2))
		idleTheme = 0;
}

void saveIdleTheme(uint8_t v)
{
	if (!(v == 0 || v == 1 || v == 2))
		v = 0;
	idleTheme = v;

	prefs.begin("wpnotif", false);
	prefs.putUChar("idleTheme", idleTheme);
	prefs.end();

	lastClockDrawMs = 0;

	lastGeoFetchMs = 0;
	geoHasData = geoManualOverride;
	geoFetching = false;

	lastWeatherFetchMs = 0;
	weatherHasData = false;
	weatherFetching = false;
}

void loadWeatherConfig()
{
	prefs.begin("wpnotif", true);
	geoManualOverride = prefs.getBool("geoManual", false);

	if (geoManualOverride)
	{
		geoLat = prefs.getFloat("geoLat", geoLat);
		geoLon = prefs.getFloat("geoLon", geoLon);
		geoCity = prefs.getString("geoCity", geoCity.c_str());
		geoTz = prefs.getString("geoTz", geoTz.c_str());
	}
	prefs.end();

	if (!geoManualOverride)
		return;

	if (!geoCity.length())
		geoCity = "GEO";
	geoCity.toUpperCase();
	if (geoCity.length() > 12)
		geoCity = geoCity.substring(0, 12);

	if (!geoTz.length())
		geoTz = "Europe/Athens";
	applyDeviceTimezone();

	geoHasData = true;
	lastGeoFetchMs = millis();
}

void saveWeatherConfig(bool manual)
{
	prefs.begin("wpnotif", false);
	prefs.putBool("geoManual", manual);

	if (manual)
	{
		prefs.putFloat("geoLat", geoLat);
		prefs.putFloat("geoLon", geoLon);
		prefs.putString("geoCity", geoCity);
		prefs.putString("geoTz", geoTz);
	}

	prefs.end();
}

const char *resolveTimezonePosix()
{
	// Default: Athens/Greece
	const char *tzPosix = "EET-2EEST,M3.5.0/3,M10.5.0/4";

	if (!geoTz.length())
		return tzPosix;

	// POSIX strings may include '/' in DST transition rules (e.g. M3.5.0/3).
	// Treat as legacy IANA only when it looks like Region/City with no comma rules.
	bool looksLikeIana = (geoTz.indexOf('/') >= 0) && (geoTz.indexOf(',') < 0);
	if (!looksLikeIana)
	{
		return geoTz.c_str();
	}

	// Backward compatibility for older IANA values saved on device.
	if (geoTz == "Europe/London" || geoTz == "Europe/Dublin" || geoTz == "Europe/Lisbon")
	{
		return "GMT0BST,M3.5.0/1,M10.5.0/2";
	}
	if (geoTz == "Europe/Athens" || geoTz == "Europe/Bucharest" || geoTz == "Europe/Sofia")
	{
		return "EET-2EEST,M3.5.0/3,M10.5.0/4";
	}
	if (geoTz == "Europe/Berlin" || geoTz == "Europe/Paris" || geoTz == "Europe/Madrid")
	{
		return "CET-1CEST,M3.5.0/2,M10.5.0/3";
	}
	return "UTC0";
}

void applyDeviceTimezone()
{
	const char *tzPosix = resolveTimezonePosix();
	setenv("TZ", tzPosix, 1);
	tzset();
}

// -------------------- Time helpers --------------------
bool timeReady()
{
	time_t now = time(nullptr);
	return now > 1700000000;
}

String humanNow()
{
	if (!timeReady())
		return "TIME:--";
	time_t now = time(nullptr);
	struct tm tm;
	localtime_r(&now, &tm);

	char buf[12];
	snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d",
			 tm.tm_mday, tm.tm_mon + 1,
			 tm.tm_hour, tm.tm_min);
	return String(buf);
}

String humanTimeHHMM()
{
	if (!timeReady())
		return "--:--";
	time_t now = time(nullptr);
	struct tm tm;
	localtime_r(&now, &tm);
	char buf[6];
	snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
	return String(buf);
}

void restampTimePlaceholdersIfReady()
{
	if (placeholdersRestamped)
		return;
	if (!timeReady())
		return;
	if (!hasMessages())
	{
		placeholdersRestamped = true;
		return;
	}

	bool changed = false;
	for (size_t i = 0; i < messageCount; i++)
	{
		size_t idx = (messageHead + i) % MESSAGE_BUFFER_SIZE;
		if (messageBuffer[idx].topic.startsWith("TIME:--"))
		{
			String t = messageBuffer[idx].topic;
			int sp = t.indexOf(' ');
			String suffix = "";
			if (sp >= 0)
				suffix = t.substring(sp);
			messageBuffer[idx].topic = humanNow() + suffix;
			changed = true;
		}
	}

	if (changed)
	{
		markHistoryDirty();
		saveHistoryToPrefsNow();
	}

	placeholdersRestamped = true;
}

// -------------------- WiFi --------------------
void startStaConnectStable()
{
	WiFi.mode(WIFI_STA);
	WiFi.setSleep(false);
	WiFi.setAutoReconnect(true);
	WiFi.begin();

	wifiConnectingSinceMs = millis();
	lastWifiAttemptMs = 0;
	wifiHardResetDone = false;
}

void hardResetWiFiStackOnce()
{
	if (wifiHardResetDone)
		return;
	wifiHardResetDone = true;

	WiFi.mode(WIFI_OFF);
	delay(WIFI_STACK_RESET_MS);

	WiFi.disconnect(true, false);
	delay(WIFI_STACK_RESET_MS);

	WiFi.mode(WIFI_STA);
	WiFi.setSleep(false);
	WiFi.setAutoReconnect(true);
	WiFi.begin();
}

// -------------------- RSSI --------------------
int16_t readRssiRaw()
{
	if (!WiFi.isConnected())
		return -999;
	return (int16_t)WiFi.RSSI();
}

void updateSmoothedRssi()
{
	unsigned long now = millis();
	if (now - lastRssiUpdateMs < RSSI_UPDATE_MS)
		return;
	lastRssiUpdateMs = now;

	int16_t raw = readRssiRaw();
	if (raw == -999)
	{
		smoothedRssi = -999;
		return;
	}

	if (smoothedRssi == -999)
		smoothedRssi = raw;
	else
		smoothedRssi = (int16_t)((RSSI_ALPHA * raw) + ((1.0f - RSSI_ALPHA) * smoothedRssi));
}

uint8_t rssiBars()
{
	if (smoothedRssi == -999)
		return 0;
	int r = smoothedRssi;
	if (r >= -50)
		return 4;
	if (r >= -60)
		return 3;
	if (r >= -70)
		return 2;
	if (r >= -80)
		return 1;
	return 0;
}

void drawRssiBars(uint8_t bars, bool dim)
{
	// Minimal icon-only signal indicator (no numeric dB text).
	const int baseX = OLED_WIDTH - 22;
	const int baseY = 11;
	const int barW = 4;
	const int gap = 1;

	for (uint8_t i = 0; i < 4; i++)
	{
		const int h = 3 + (i * 2);
		const int x = baseX + (i * (barW + gap));
		const int y = baseY - h + 1;

		if (i < bars)
		{
			if (dim)
				display.drawRect(x, y, barW, h, SSD1306_WHITE);
			else
				display.fillRect(x, y, barW, h, SSD1306_WHITE);
		}
		else
		{
			display.drawRect(x, y, barW, h, SSD1306_WHITE);
		}
	}

	(void)dim;
}

// -------------------- History persistence --------------------
/** Reset runtime history without modifying the saved NVS blob. */
void clearHistoryInRam()
{
	messageCount = 0;
	messageHead = 0;
	currentIndex = 0;
	historyActive = false;

	for (size_t i = 0; i < MESSAGE_BUFFER_SIZE; i++)
	{
		messageBuffer[i].topic = "";
		messageBuffer[i].payload = "";
		messageBuffer[i].read = true;
	}
}

/** Permanently remove the serialized history blob from NVS. */
void wipeHistoryPrefs()
{
	prefs.begin("wpnotif", false);
	prefs.remove("hist");
	prefs.end();
}

/**
 * Schedule a delayed history write.
 *
 * Read/unread navigation can generate several mutations close together. The
 * delay coalesces those changes and reduces NVS wear.
 */
void markHistoryDirty()
{
	if (!historyDirty)
	{
		historyDirty = true;
		historyDirtySinceMs = millis();
	}
}

/** Flush a scheduled history write after HISTORY_SAVE_DELAY_MS. */
void maybeSaveHistory()
{
	if (!historyDirty)
		return;
	if (historyDirtySinceMs == 0)
		historyDirtySinceMs = millis();
	if (millis() - historyDirtySinceMs >= HISTORY_SAVE_DELAY_MS)
	{
		saveHistoryToPrefsNow();
	}
}

/**
 * Serialize the logical ring order into a versioned, packed NVS record.
 *
 * Entries are saved oldest-to-newest with a normalized head of zero. The
 * current physical index is converted to a logical position before storage.
 */
void saveHistoryToPrefsNow()
{
	PersistHistory ph;
	memset(&ph, 0, sizeof(ph));
	ph.magic = HISTORY_MAGIC;
	ph.version = HISTORY_VERSION;

	uint8_t countToSave = (messageCount > MESSAGE_BUFFER_SIZE)
							  ? (uint8_t)MESSAGE_BUFFER_SIZE
							  : (uint8_t)messageCount;

	ph.count = countToSave;
	ph.head = 0;

	uint8_t curPos = 0;
	if (countToSave > 0)
	{
		size_t pos = (currentIndex + MESSAGE_BUFFER_SIZE - messageHead) % MESSAGE_BUFFER_SIZE;
		if (pos >= countToSave)
			pos = 0;
		curPos = (uint8_t)pos;
	}
	ph.current = curPos;

	for (uint8_t i = 0; i < countToSave; i++)
	{
		size_t idx = (messageHead + i) % MESSAGE_BUFFER_SIZE;
		safeCopyToC(ph.msgs[i].topic, sizeof(ph.msgs[i].topic), messageBuffer[idx].topic);
		safeCopyToC(ph.msgs[i].payload, sizeof(ph.msgs[i].payload), messageBuffer[idx].payload);
		ph.msgs[i].read = messageBuffer[idx].read ? 1 : 0;
	}

	for (uint8_t i = countToSave; i < MESSAGE_BUFFER_SIZE; i++)
	{
		ph.msgs[i].topic[0] = '\0';
		ph.msgs[i].payload[0] = '\0';
		ph.msgs[i].read = 1;
	}

	prefs.begin("wpnotif", false);
	prefs.putBytes("hist", &ph, sizeof(ph));
	prefs.end();

	historyDirty = false;
	historyDirtySinceMs = 0;
}

/**
 * Restore and validate the versioned history record from NVS.
 *
 * Unknown versions, invalid sizes, and corrupt counts fail closed to an empty
 * history rather than attempting a partial migration.
 */
void loadHistoryFromPrefs()
{
	PersistHistory ph;
	memset(&ph, 0, sizeof(ph));

	prefs.begin("wpnotif", true);
	size_t gotLen = prefs.getBytesLength("hist");
	if (gotLen != sizeof(PersistHistory))
	{
		prefs.end();
		clearHistoryInRam();
		return;
	}
	prefs.getBytes("hist", &ph, sizeof(ph));
	prefs.end();

	if (ph.magic != HISTORY_MAGIC || ph.version != HISTORY_VERSION)
	{
		clearHistoryInRam();
		return;
	}

	uint8_t count = ph.count;
	if (count == 0 || count > MESSAGE_BUFFER_SIZE)
	{
		clearHistoryInRam();
		return;
	}

	for (size_t i = 0; i < MESSAGE_BUFFER_SIZE; i++)
	{
		messageBuffer[i].topic = "";
		messageBuffer[i].payload = "";
		messageBuffer[i].read = true;
	}

	messageHead = 0;
	messageCount = count;
	historyActive = false;

	for (uint8_t i = 0; i < count; i++)
	{
		messageBuffer[i].topic = String(ph.msgs[i].topic);
		messageBuffer[i].payload = String(ph.msgs[i].payload);
		messageBuffer[i].read = (ph.msgs[i].read != 0);
	}

	uint8_t curPos = ph.current;
	if (curPos >= count)
		curPos = 0;
	currentIndex = curPos;

	focusFirstUnreadIfAny();
}

bool findFirstUnreadIndex(size_t &outIdx)
{
	if (messageCount == 0)
		return false;
	for (size_t i = 0; i < messageCount; i++)
	{
		size_t idx = (messageHead + i) % MESSAGE_BUFFER_SIZE;
		if (!messageBuffer[idx].read)
		{
			outIdx = idx;
			return true;
		}
	}
	return false;
}

void focusFirstUnreadIfAny()
{
	size_t idx = 0;
	if (findFirstUnreadIndex(idx))
	{
		currentIndex = idx;
		historyActive = false;
	}
}

// -------------------- Status bar --------------------
bool hasMessages() { return messageCount > 0; }

uint16_t unreadCount()
{
	uint16_t c = 0;
	for (size_t i = 0; i < messageCount; i++)
	{
		size_t idx = (messageHead + i) % MESSAGE_BUFFER_SIZE;
		if (!messageBuffer[idx].read)
			c++;
	}
	return c;
}

void updateHeartbeat()
{
	unsigned long now = millis();
	if (now - lastHeartbeatMs >= 950)
	{
		lastHeartbeatMs = now;
		heartbeatOn = !heartbeatOn;
	}
}

void updateUnreadBlink()
{
	unsigned long now = millis();
	if (now - lastUnreadBlinkMs >= 500)
	{
		lastUnreadBlinkMs = now;
		unreadBlinkOn = !unreadBlinkOn;
	}
}

void drawHeartbeatLeftBig(bool dim)
{
	// Keep a little breathing room from the top edge.
	const int cx = 5, cy = 6;
	const int r = 3;

	if (heartbeatOn)
	{
		display.fillCircle(cx, cy, r, SSD1306_WHITE);
	}
	else
	{
		display.drawCircle(cx, cy, r, SSD1306_WHITE);
	}
}

void drawUnreadEnvelopeMid(bool dim)
{
	unsigned long now = millis();
	if (now < receivingUntilMs)
	{
		display.setTextSize(1);
		display.setTextColor(SSD1306_WHITE);
		const char *badge = "RECEIVING";
		int16_t x1, y1;
		uint16_t w, h;
		display.getTextBounds(badge, 0, 0, &x1, &y1, &w, &h);
		int cx = ((OLED_WIDTH - (int)w) / 2) - x1;
		display.setCursor(cx, 2);
		display.print(badge);
		(void)dim;
		return;
	}

	const uint16_t count = unreadCount();
	if (count == 0)
	{
		if (mqttConfigValid)
		{
			display.setTextSize(1);
			display.setCursor(49, 2);
			display.print(mqttClient.connected() ? "MQTT" : "WAIT");
		}
		return;
	}

	updateUnreadBlink();
	if (!unreadBlinkOn)
		return;

	const int w = 14, h = 9;
	const int x = 51;
	const int y = 2;

	display.drawRect(x, y, w, h, SSD1306_WHITE);
	display.drawLine(x, y, x + w / 2, y + h / 2, SSD1306_WHITE);
	display.drawLine(x + w - 1, y, x + w / 2, y + h / 2, SSD1306_WHITE);
	display.drawLine(x, y + h - 1, x + w / 2, y + h / 2, SSD1306_WHITE);
	display.drawLine(x + w - 1, y + h - 1, x + w / 2, y + h / 2, SSD1306_WHITE);

	char countLabel[5];
	snprintf(countLabel, sizeof(countLabel), "%u", count > 99 ? 99 : count);
	display.setTextSize(1);
	display.setCursor(69, 2);
	display.print(countLabel);

	(void)dim;
}

void drawStatusBar(bool dim)
{
	updateSmoothedRssi();
	updateHeartbeat();

	display.setTextSize(1);
	display.setTextColor(SSD1306_WHITE);
	drawHeartbeatLeftBig(dim);
	drawUnreadEnvelopeMid(dim);
	drawRssiBars(rssiBars(), dim);
}

// -------------------- Message rendering --------------------
static uint8_t chooseTextSize(const String &message)
{
	return (message.length() <= 24) ? 2 : 1;
}

static uint8_t maxCharsForSize(uint8_t textSize)
{
	return (textSize == 2) ? 10 : 21;
}

void resetMessageFlipState(size_t idx)
{
	flipIndex = idx;
	showTitlePhase = true;
	lastFlipMs = millis();
}

// Draw one message frame with word-wrap, title/body phase logic, and optional paging.
void drawWrappedMessage(const String &topic, const String &message, bool unreadMark)
{
	if (flipIndex != currentIndex)
		resetMessageFlipState(currentIndex);

	String title = message;
	String body = "";
	int sep = message.indexOf('|');
	if (sep >= 0)
	{
		title = message.substring(0, sep);
		body = message.substring(sep + 1);
		int sep2 = body.indexOf('|');
		if (sep2 >= 0)
			body = body.substring(0, sep2);
	}

	String displayText;
	bool bodyPhaseVisible = false;
	if (title.length() && body.length())
	{
		unsigned long now = millis();
		unsigned long phaseMs = showTitlePhase ? MESSAGE_TITLE_MS : MESSAGE_BODY_MS;
		if (now - lastFlipMs >= phaseMs)
		{
			showTitlePhase = !showTitlePhase;
			lastFlipMs = now;
		}

		bodyPhaseVisible = !showTitlePhase;
		displayText = bodyPhaseVisible ? body : title;
	}
	else if (title.length())
	{
		displayText = title;
	}
	else
	{
		displayText = body;
	}
	if (!displayText.length())
		displayText = "(empty)";

	// Titles get visual emphasis; detail text stays compact so useful context
	// fits on the 128x64 display without frantic paging.
	const uint8_t textSize = bodyPhaseVisible ? 1 : chooseTextSize(displayText);
	const uint8_t maxChars = maxCharsForSize(textSize);
	const uint8_t maxLines = (textSize == 2)
								 ? 2
								 : 4;

	display.clearDisplay();
	drawStatusBar(false);

	display.setTextSize(1);
	display.setTextColor(SSD1306_WHITE);
	char positionLabel[10];
	size_t position = (currentIndex + MESSAGE_BUFFER_SIZE - messageHead) % MESSAGE_BUFFER_SIZE;
	if (position >= messageCount)
		position = 0;
	snprintf(positionLabel, sizeof(positionLabel), "%u/%u", (unsigned)(position + 1), (unsigned)messageCount);
	int16_t lx1, ly1;
	uint16_t lw, lh;
	display.getTextBounds(positionLabel, 0, 0, &lx1, &ly1, &lw, &lh);
	int stateX = ((int)OLED_WIDTH - (int)lw - 1) - lx1;

	// Keep topic text from colliding with the right-side read/unread badge.
	String header = topic;
	int headerMaxPx = stateX - 6;
	while (header.length() > 0)
	{
		int16_t hx1, hy1;
		uint16_t hw, hh;
		display.getTextBounds(header.c_str(), 0, 0, &hx1, &hy1, &hw, &hh);
		if ((int)hw <= headerMaxPx)
			break;
		header.remove(header.length() - 1);
	}

	display.setCursor(0, 16);
	display.print(header);
	display.setCursor(stateX, 16);
	display.print(positionLabel);

	// Per-message unread marker so unread items are identifiable while browsing.
	const int unreadX = stateX - 5;
	const int unreadY = 19;
	if (unreadMark)
		display.fillCircle(unreadX, unreadY, 2, SSD1306_WHITE);
	else
		display.drawCircle(unreadX, unreadY, 2, SSD1306_WHITE);

	display.drawFastHLine(0, 26, OLED_WIDTH, SSD1306_WHITE);
	display.setTextSize(textSize);
	display.setCursor(0, 30);

	static const uint8_t MAX_WRAPPED_LINES = 10;
	String wrappedLines[MAX_WRAPPED_LINES];
	uint8_t wrappedCount = 0;

	auto appendWrappedLine = [&](const String &srcLine)
	{
		String rest = srcLine;
		while (rest.length() && wrappedCount < MAX_WRAPPED_LINES)
		{
			if (rest.length() <= maxChars)
			{
				wrappedLines[wrappedCount++] = rest;
				break;
			}

			int cut = maxChars;
			while (cut > 0 && rest[cut - 1] != ' ' && rest[cut - 1] != '\t')
				cut--;
			if (cut <= 0)
				cut = maxChars; // no whitespace found; hard split

			String chunk = rest.substring(0, cut);
			chunk.trim();
			if (chunk.length())
			{
				wrappedLines[wrappedCount++] = chunk;
			}

			int nextStart = cut;
			while (nextStart < (int)rest.length() && (rest[nextStart] == ' ' || rest[nextStart] == '\t'))
				nextStart++;
			rest = (nextStart < (int)rest.length()) ? rest.substring(nextStart) : String("");
		}
	};

	int start = 0;
	while (start < (int)displayText.length() && wrappedCount < MAX_WRAPPED_LINES)
	{
		int nl = displayText.indexOf('\n', start);
		String part = (nl >= 0) ? displayText.substring(start, nl) : displayText.substring(start);
		start = (nl >= 0) ? (nl + 1) : (int)displayText.length();

		if (!part.length())
		{
			wrappedLines[wrappedCount++] = "";
			continue;
		}
		appendWrappedLine(part);
	}

	size_t startLine = 0;
	if (bodyPhaseVisible && wrappedCount > maxLines)
	{
		const uint8_t pageStride = maxLines;
		const unsigned long pageMs = 2600;
		size_t pages = 1 + (wrappedCount - maxLines + pageStride - 1) / pageStride;
		size_t page = (millis() / pageMs) % pages;
		startLine = page * pageStride;
		if (startLine + maxLines > wrappedCount)
		{
			startLine = wrappedCount - maxLines;
		}
	}

	for (size_t i = startLine; i < wrappedCount && i < (startLine + maxLines); i++)
	{
		display.println(wrappedLines[i]);
	}

	display.display();
}

void showMessageAt(size_t idx, bool markRead)
{
	if (!hasMessages())
		return;

	idx %= MESSAGE_BUFFER_SIZE;
	currentIndex = idx;
	resetMessageFlipState(currentIndex);

	bool isUnread = !messageBuffer[currentIndex].read;
	if (markRead && isUnread)
	{
		messageBuffer[currentIndex].read = true;
		saveHistoryToPrefsNow();
		isUnread = false;
	}

	drawWrappedMessage(messageBuffer[currentIndex].topic, messageBuffer[currentIndex].payload, isUnread);
	bumpNoIdleGuard();
}

void showCurrentMessage(bool markRead)
{
	if (!hasMessages())
		return;
	showMessageAt(currentIndex, markRead);
}

void showNoMessagesOverlay()
{
	showNoMessagesUntilMs = millis() + NO_MESSAGES_DISPLAY_MS;
	// Keep the short feedback visible, then allow idle immediately.
	lastUserOrMsgMs = millis() - (IDLE_AFTER_MS + 1000);
	noIdleUntilMs = millis();
	drawNoMessagesScreen();
}

void drawNoMessagesScreen()
{
	drawCenteredText("NO", "MESSAGES", 2);
}

void gotoNextMessage(bool markRead)
{
	if (!hasMessages())
	{
		showNoMessagesOverlay();
		return;
	}

	historyActive = true;

	size_t pos = (currentIndex + MESSAGE_BUFFER_SIZE - messageHead) % MESSAGE_BUFFER_SIZE;
	if (pos >= messageCount)
		pos = 0;

	pos = (pos + 1) % messageCount;
	size_t idx = (messageHead + pos) % MESSAGE_BUFFER_SIZE;
	showMessageAt(idx, markRead);
}

void gotoPrevMessage(bool markRead)
{
	if (!hasMessages())
	{
		showNoMessagesOverlay();
		return;
	}

	historyActive = true;

	size_t pos = (currentIndex + MESSAGE_BUFFER_SIZE - messageHead) % MESSAGE_BUFFER_SIZE;
	if (pos >= messageCount)
		pos = 0;

	pos = (pos + messageCount - 1) % messageCount;
	size_t idx = (messageHead + pos) % MESSAGE_BUFFER_SIZE;
	showMessageAt(idx, markRead);
}

void markCurrentReadAndPersist()
{
	if (!hasMessages())
	{
		showNoMessagesOverlay();
		return;
	}

	if (!messageBuffer[currentIndex].read)
	{
		messageBuffer[currentIndex].read = true;
		saveHistoryToPrefsNow();
	}
	bumpNoIdleGuard();
	showCurrentMessage(false);
}

void toggleCurrentReadStateAndPersist()
{
	if (!hasMessages())
	{
		showNoMessagesOverlay();
		return;
	}

	messageBuffer[currentIndex].read = !messageBuffer[currentIndex].read;
	saveHistoryToPrefsNow();
	uiFeedback = messageBuffer[currentIndex].read ? UiFeedback::MarkedRead : UiFeedback::MarkedUnread;
	uiFeedbackUntilMs = millis() + UI_FEEDBACK_MS;

	bumpNoIdleGuard();
	drawGestureFeedback();
}

void markAllReadAndPersist()
{
	if (!hasMessages())
		return;

	bool changed = false;
	for (size_t i = 0; i < messageCount; i++)
	{
		size_t idx = (messageHead + i) % MESSAGE_BUFFER_SIZE;
		if (!messageBuffer[idx].read)
		{
			messageBuffer[idx].read = true;
			changed = true;
		}
	}
	if (changed)
		saveHistoryToPrefsNow();

	uiFeedback = UiFeedback::AllRead;
	uiFeedbackUntilMs = millis() + UI_FEEDBACK_MS;
	bumpNoIdleGuard();
	drawGestureFeedback();
}

void drawGestureFeedback()
{
	switch (uiFeedback)
	{
	case UiFeedback::MarkedRead:
		drawCenteredText("MARKED", "READ", 2);
		break;
	case UiFeedback::MarkedUnread:
		drawCenteredText("MARKED", "UNREAD", 2);
		break;
	case UiFeedback::AllRead:
		drawCenteredText("ALL", "READ", 2);
		break;
	default:
		break;
	}
}

void clearAllMessagesAndShowFeedback()
{
	// History deletion is intentionally available only through the MQTT command.
	clearHistoryInRam();
	wipeHistoryPrefs();
	placeholdersRestamped = false;
	resetMessageFlipState(0);
	drawCenteredText("CLEARED", "MSGS", 2);
	flashOnboardLedColor(255, 120, 0, 50);
	flashOnboardLedColor(255, 120, 0, 50);

	// No messages left: make idle eligible immediately.
	lastUserOrMsgMs = millis() - (IDLE_AFTER_MS + 1000);
	noIdleUntilMs = millis();
}

// -------------------- Push message --------------------
static String normalizeType(const String &type)
{
	if (!type.length())
		return "";
	if (type == "generic_notification")
		return "GEN";
	if (type == "warning" || type == "warn")
		return "WARN";
	if (type == "error" || type == "err")
		return "ERR";
	if (type == "info")
		return "INFO";
	return type.length() > 6 ? type.substring(0, 6) : type;
}

static String makeHeaderWithType(const String &type)
{
	String t = humanNow();
	String tt = normalizeType(type);
	if (tt.length())
		t += " " + tt;
	return t;
}

/**
 * Insert a notification into the bounded ring and persist it immediately.
 *
 * When the ring is full, the oldest entry is replaced. Text is truncated to
 * the fixed NVS limits before it enters runtime state, keeping what the user
 * sees consistent with what will survive a restart.
 */
void pushMessage(const String &header, const String &payload)
{
	size_t insertIndex;

	if (messageCount < MESSAGE_BUFFER_SIZE)
	{
		insertIndex = (messageHead + messageCount) % MESSAGE_BUFFER_SIZE;
		messageCount++;
	}
	else
	{
		insertIndex = messageHead;
		messageHead = (messageHead + 1) % MESSAGE_BUFFER_SIZE;
	}

	String t = header.length() ? header : makeHeaderWithType("");
	String p = payload;

	if (t.length() >= (MAX_TOPIC_CHARS - 1))
		t = t.substring(0, MAX_TOPIC_CHARS - 1);
	if (p.length() >= (MAX_PAYLOAD_CHARS - 1))
		p = p.substring(0, MAX_PAYLOAD_CHARS - 1);

	messageBuffer[insertIndex] = {t, p, false};
	currentIndex = insertIndex;
	resetMessageFlipState(currentIndex);

	historyActive = false;
	bumpNoIdleGuard();

	saveHistoryToPrefsNow();
}

// -------------------- Setup portal screens --------------------
void drawBootWelcomeScreen()
{
	display.clearDisplay();
	display.setTextColor(SSD1306_WHITE);

	auto centerX = [&](const char *s, uint8_t ts) -> int
	{
		int16_t x1, y1;
		uint16_t w, h;
		display.setTextSize(ts);
		display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
		return ((OLED_WIDTH - (int)w) / 2) - x1;
	};

	display.setTextSize(2);
	display.setCursor(centerX("WELCOME", 2), 0);
	display.println("WELCOME");

	// Larger second-line text split in two lines so it remains readable on 128px width.
	display.setTextSize(2);
	display.setCursor(centerX("TO", 2), 22);
	display.println("TO");
	// Keep a single-word NOTIFICATOR line while still looking large on 128px.
	// Size 2 default spacing clips, so render per-character with tighter spacing.
	const char *notif = "NOTIFICATOR";
	const int notifLen = 11;
	const int charW = 12; // default font width at text size 2
	const int step = 11;  // tightened spacing so the full word fits
	const int totalW = charW + ((notifLen - 1) * step);
	int notifX = (OLED_WIDTH - totalW) / 2;
	int notifY = 44;

	display.setTextSize(2);
	for (int i = 0; i < notifLen; i++)
	{
		display.setCursor(notifX + (i * step), notifY);
		display.write(notif[i]);
	}

	display.display();
}

void drawSetupInstructions()
{
	static unsigned long lastDrawMs = 0;
	unsigned long now = millis();
	if (now - lastDrawMs < 250)
		return;
	lastDrawMs = now;

	display.clearDisplay();
	display.setTextColor(SSD1306_WHITE);

	display.setTextSize(2);
	display.setCursor(0, 0);
	display.println("SETUP MODE");

	display.setTextSize(1);
	display.setCursor(0, 18);
	display.println("Connect to WiFi:");

	String ssid = apSsid;
	if (!ssid.length())
		ssid = "(starting...)";

	const int maxChars = 20;
	if (ssid.length() > maxChars)
	{
		ssid = ssid.substring(0, maxChars - 3) + "...";
	}

	display.setCursor(0, 30);
	display.print("SSID: ");
	display.println(ssid);

	IPAddress ip = WiFi.softAPIP();
	display.setCursor(0, 44);
	if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0)
	{
		display.println("IP: starting...");
	}
	else
	{
		char ipBuf[20];
		snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
		display.print("IP: ");
		display.println(ipBuf);
	}

	display.setCursor(0, 56);
	display.println("Open IP in browser");

	display.display();
}

void drawPortalAnimationFrame()
{
	unsigned long now = millis();
	if (now - animLastMs < 120)
		return;
	animLastMs = now;
	animFrame++;

	display.clearDisplay();

	display.setTextSize(2);
	display.setTextColor(SSD1306_WHITE);
	display.setCursor(0, 0);
	display.println("SETUP");
	display.setTextSize(1);
	display.setCursor(0, 18);
	display.println("IN PROGRESS");

	const int cx = OLED_WIDTH - 10;
	const int cy = 10;
	const int r = 4;

	for (int i = 0; i < 8; i++)
	{
		float a = (float)i * 0.785398f;
		int x = cx + (int)(cosf(a) * r);
		int y = cy + (int)(sinf(a) * r);
		if (i == (animFrame % 8))
			display.fillCircle(x, y, 1, SSD1306_WHITE);
		else
			display.drawPixel(x, y, SSD1306_WHITE);
	}

	display.setCursor(0, 40);
	display.println("Config portal active");
	display.setCursor(0, 50);
	display.println("Saving settings...");

	display.display();
}

/**
 * Start the non-blocking WiFiManager portal.
 *
 * Existing secret values remain in NVS and are never copied into HTML fields.
 * The main loop continues servicing this portal through wm.process().
 */
void startSetupPortal()
{
	syncMqttPortalParameters();
	mqttConfigSaveRequested = false;
	drawSetupInstructions();
	setupScreenDrawn = false;
	flashOnboardLedColor(170, 60, 255, 70);

	WiFi.disconnect(true, false);
	delay(150);

	WiFi.mode(WIFI_AP_STA);
	delay(150);

	wm.setConfigPortalBlocking(false);

	portalRunning = true;
	portalStartMs = millis();
	portalStartChecked = false;

	wifiConnectingSinceMs = 0;
	lastWifiAttemptMs = 0;
	wifiHardResetDone = false;

	if (WIFI_AP_PASSWORD[0] != '\0')
		wm.startConfigPortal(apSsid.c_str(), WIFI_AP_PASSWORD);
	else
		wm.startConfigPortal(apSsid.c_str());

	bumpNoIdleGuard();
}

/**
 * Validate portal results and transition back to station/MQTT operation.
 *
 * Invalid or incomplete broker settings reopen the portal with clear OLED
 * feedback instead of leaving the device in a partially configured state.
 */
void finalizeSetupAfterPortal()
{
	if (mqttConfigSaveRequested && !saveMqttConfigFromPortal())
	{
		drawCenteredText("MQTT", "CHECK SETUP", 2);
		delay(1200);
		startSetupPortal();
		return;
	}
	if (!mqttConfigValid)
	{
		drawCenteredText("MQTT", "SETUP NEEDED", 2);
		delay(1200);
		startSetupPortal();
		return;
	}

	saveConfiguredFlag(true);
	deviceConfigured = true;
	saveLastSsidForInfo(WiFi.SSID());

	drawCenteredText("SAVING", "SETTINGS", 2);

	lastMqttAttemptMs = 0;
	mqttClient.disconnect();
	setupMqttClient();
	bumpNoIdleGuard();
	flashOnboardLedColor(0, 200, 140, 80);

	portalRunning = false;
	WiFi.mode(WIFI_STA);
	startStaConnectStable();
}

// -------------------- Device ID screen --------------------
void drawDeviceIdScreen()
{
	display.clearDisplay();
	drawStatusBar(false);

	auto centerX1 = [&](const char *s, uint8_t ts) -> int
	{
		int16_t x1, y1;
		uint16_t w, h;
		display.setTextSize(ts);
		display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
		return ((OLED_WIDTH - (int)w) / 2) - x1;
	};

	display.setTextColor(SSD1306_WHITE);
	const bool connectionPage = ((millis() / 2800) % 2) == 1;

	if (!connectionPage)
	{
		display.setTextSize(1);
		display.setCursor(centerX1("DEVICE ID", 1), 14);
		display.print("DEVICE ID");

		String shortId = deviceId;
		if (shortId.length() > 8)
			shortId = shortId.substring(shortId.length() - 8);
		shortId.toUpperCase();
		display.setTextSize(2);
		display.setCursor(centerX1(shortId.c_str(), 2), 25);
		display.print(shortId);

		char fwBuf[24];
		snprintf(fwBuf, sizeof(fwBuf), "FIRMWARE %s", FW_VERSION);
		display.setTextSize(1);
		display.setCursor(centerX1(fwBuf, 1), 50);
		display.print(fwBuf);
	}
	else
	{
		display.setTextSize(1);
		display.setCursor(centerX1("CONNECTION", 1), 14);
		display.print("CONNECTION");

		const char *connection = !WiFi.isConnected()
									 ? "NO WIFI"
									 : (mqttClient.connected() ? "MQTT READY" : "MQTT WAIT");
		display.setTextSize(2);
		display.setCursor(centerX1(connection, 2), 27);
		display.print(connection);

		String hostLabel = mqttConfigValid ? mqttHost : String("SETUP REQUIRED");
		if (hostLabel.length() > 20)
			hostLabel = hostLabel.substring(0, 17) + "...";
		display.setTextSize(1);
		display.setCursor(centerX1(hostLabel.c_str(), 1), 52);
		display.print(hostLabel);
	}

	display.display();
}

void drawHoldCounter(unsigned long heldMs)
{
	static unsigned long lastDrawMs = 0;
	unsigned long now = millis();
	if (now - lastDrawMs < 120)
		return;
	lastDrawMs = now;

	display.clearDisplay();
	drawStatusBar(false);
	display.setTextColor(SSD1306_WHITE);

	display.setTextSize(1);
	const char *holdLabel = "TOUCH ACTION";
	int16_t x1, y1;
	uint16_t w, h;
	display.getTextBounds(holdLabel, 0, 0, &x1, &y1, &w, &h);
	display.setCursor(((OLED_WIDTH - (int)w) / 2) - x1, 16);
	display.println(holdLabel);

	unsigned long whole = heldMs / 1000;
	unsigned long tenths = (heldMs % 1000) / 100;
	char secBuf[16];
	snprintf(secBuf, sizeof(secBuf), "%lu.%lus", whole, tenths);

	display.setTextSize(3);
	display.getTextBounds(secBuf, 0, 0, &x1, &y1, &w, &h);
	display.setCursor(((OLED_WIDTH - (int)w) / 2) - x1, 24);
	display.print(secBuf);

	display.setTextSize(1);
	const char *action = (heldMs >= BUTTON_SETUP_LONG_PRESS_MS)
							 ? "RELEASE: SETUP"
							 : ((heldMs >= BUTTON_LONG_PRESS_MS)
									? "RELEASE: READ ALL"
									: "KEEP HOLDING");
	display.getTextBounds(action, 0, 0, &x1, &y1, &w, &h);
	display.setCursor(((OLED_WIDTH - (int)w) / 2) - x1, 56);
	display.print(action);

	display.display();
}

// -------------------- Weather --------------------
const char *weatherCodeToShort(uint8_t code)
{
	// Open-Meteo weathercode mapping (compact)
	switch (code)
	{
	case 0:
		return "CLR";
	case 1:
		return "MCLR";
	case 2:
		return "PCLD";
	case 3:
		return "CLD";
	case 45:
	case 48:
		return "FOG";
	case 51:
	case 53:
	case 55:
		return "DRZ";
	case 61:
	case 63:
	case 65:
		return "RAIN";
	case 66:
	case 67:
		return "FRZR";
	case 71:
	case 73:
	case 75:
		return "SNOW";
	case 77:
		return "SGRN";
	case 80:
	case 81:
	case 82:
		return "SHWR";
	case 85:
	case 86:
		return "SNSH";
	case 95:
		return "TSTM";
	case 96:
	case 99:
		return "HAIL";
	default:
		return "WX";
	}
}

/**
 * Resolve approximate location and timezone from the network's public IP.
 *
 * This request is made only for weather-capable idle themes and is skipped
 * after a manual location override. No device ID or Notificator credential is
 * included in the request.
 */
bool fetchGeoByIPNow()
{
	if (!WiFi.isConnected())
		return false;

	WiFiClient client;

	HTTPClient http;
	if (!http.begin(client, "http://ip-api.com/json/"))
		return false;

	int code = http.GET();
	if (code != 200)
	{
		http.end();
		return false;
	}

	String body = http.getString();
	http.end();

	StaticJsonDocument<1536> doc;
	if (deserializeJson(doc, body) != DeserializationError::Ok)
		return false;

	const char *status = doc["status"] | "";
	if (strcmp(status, "success") != 0)
		return false;

	// ip-api.com fields: city, lat, lon, timezone
	float lat = doc["lat"] | 0.0f;
	float lon = doc["lon"] | 0.0f;
	const char *city = doc["city"] | "";
	const char *tz = doc["timezone"] | "Europe/Athens";

	if (lat == 0.0f && lon == 0.0f)
		return false;

	geoLat = lat;
	geoLon = lon;

	if (city && city[0])
	{
		geoCity = String(city);
		geoCity.toUpperCase();
		if (geoCity.length() > 12)
			geoCity = geoCity.substring(0, 12);
	}
	else
	{
		geoCity = "GEO";
	}

	geoTz = String(tz);

	geoHasData = true;
	return true;
}

void maybeFetchGeoByIP()
{
	if (!(idleTheme == 1 || idleTheme == 2))
		return;
	if (!WiFi.isConnected())
		return;
	if (geoManualOverride && geoHasData)
		return;

	unsigned long now = millis();
	if (geoFetching)
		return;

	bool due = (lastGeoFetchMs == 0) || (now - lastGeoFetchMs >= GEO_REFRESH_MS);
	if (!due && geoHasData)
		return;

	geoFetching = true;
	bool ok = fetchGeoByIPNow();
	lastGeoFetchMs = now;
	geoFetching = false;

	if (!ok)
	{
		geoHasData = false;
		geoCity = "GEO";
		return;
	}

	// Update TZ if geo fetch succeeded (ESP32 expects POSIX TZ)
	if (geoTz.length())
	{
		applyDeviceTimezone();
	}
}

/**
 * Fetch current conditions for the active coordinates from Open-Meteo.
 *
 * The request contains latitude, longitude, and timezone only. No credentials
 * or notification data are sent. TLS certificate verification is currently
 * omitted to avoid another trust anchor in the constrained firmware image.
 */
bool fetchWeatherNow()
{
	if (!WiFi.isConnected())
		return false;

	WiFiClientSecure client;
	client.setInsecure();

	HTTPClient http;

	// Open-Meteo expects IANA timezone names; POSIX TZ strings break requests.
	String weatherTz = "auto";
	bool looksLikeIana = (geoTz.indexOf('/') >= 0) && (geoTz.indexOf(',') < 0);
	if (looksLikeIana)
	{
		weatherTz = geoTz;
	}

	// URL-encode timezone value (IANA paths include '/').
	String tzEnc = weatherTz;
	tzEnc.replace("/", "%2F");
	String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(geoLat, 4) +
				 "&longitude=" + String(geoLon, 4) +
				 "&current=temperature_2m,wind_speed_10m,weather_code" +
				 "&timezone=" + tzEnc;

	if (!http.begin(client, url))
		return false;

	int code = http.GET();
	if (code != 200)
	{
		http.end();
		return false;
	}

	String body = http.getString();
	http.end();

	StaticJsonDocument<1536> doc;
	if (deserializeJson(doc, body) != DeserializationError::Ok)
		return false;

	JsonObject cur = doc["current"];
	if (cur.isNull())
		return false;

	weatherTempC = cur["temperature_2m"] | weatherTempC;
	weatherWindKmh = cur["wind_speed_10m"] | weatherWindKmh;
	weatherCode = (uint8_t)(cur["weather_code"] | weatherCode);

	weatherHasData = true;
	return true;
}

void maybeFetchWeather()
{
	if (!(idleTheme == 1 || idleTheme == 2))
		return;
	if (!WiFi.isConnected())
		return;

	// Only trigger geo lookups when weather theme is selected.
	maybeFetchGeoByIP();
	if (!geoHasData)
		return;

	unsigned long now = millis();
	if (weatherFetching)
		return;

	bool due = (lastWeatherFetchMs == 0) || (now - lastWeatherFetchMs >= WEATHER_REFRESH_MS);
	if (!due)
		return;

	weatherFetching = true;
	bool ok = fetchWeatherNow();
	lastWeatherFetchMs = now;
	weatherFetching = false;

	(void)ok;
}

void drawIdleWeatherFrame()
{
	maybeFetchWeather();

	display.clearDisplay();
	drawStatusBar(true);

	display.setTextColor(SSD1306_WHITE);

	// main: temp
	display.setTextSize(4);
	String t = weatherHasData ? (String((int)round(weatherTempC)) + "C") : String("--C");
	int16_t x1, y1;
	uint16_t w, h;
	display.getTextBounds(t.c_str(), 0, 0, &x1, &y1, &w, &h);
	display.setCursor((OLED_WIDTH - (int)w) / 2, 16);
	display.print(t);

	// bottom: condition + wind, centered
	display.setTextSize(2);
	String bottomLine;
	if (!WiFi.isConnected())
	{
		bottomLine = "NO WIFI";
	}
	else if (geoFetching)
	{
		bottomLine = "LOCATING...";
	}
	else if (weatherFetching)
	{
		bottomLine = "FETCHING...";
	}
	else if (!weatherHasData)
	{
		bottomLine = "NO DATA";
	}
	else
	{
		bottomLine = String(weatherCodeToShort(weatherCode));
	}

	int16_t bx1, by1;
	uint16_t bw, bh;
	display.getTextBounds(bottomLine.c_str(), 0, 0, &bx1, &by1, &bw, &bh);
	display.setCursor((OLED_WIDTH - (int)bw) / 2, 48);
	display.print(bottomLine);

	display.display();
}

// -------------------- Commands --------------------
/**
 * Publish retained device health and OTA lifecycle information.
 *
 * Core fields are always included. Status, targetVersion, and error are added
 * only when supplied by the caller.
 *
 * @return false when MQTT is unavailable or publish fails.
 */
bool publishDeviceStatus(const char *eventName, const char *status, const String &targetVersion, const String &error)
{
	if (!mqttClient.connected() || !mqttStatusTopic.length())
		return false;

	StaticJsonDocument<320> doc;
	doc["type"] = "device_status";
	doc["event"] = (eventName && eventName[0]) ? eventName : "status";
	doc["deviceId"] = deviceId;
	doc["firmware"] = FW_VERSION;
	doc["uptime"] = millis() / 1000UL;
	doc["freeHeap"] = ESP.getFreeHeap();
	doc["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : -999;
	if (status && status[0])
		doc["status"] = status;
	if (targetVersion.length())
		doc["targetVersion"] = targetVersion;
	if (error.length())
		doc["error"] = error;

	String payload;
	payload.reserve(280);
	serializeJson(doc, payload);
	return mqttClient.publish(mqttStatusTopic.c_str(), payload.c_str(), true);
}

namespace
{
/** Accept only the two release channels supported by the public manifest. */
String normalizeOtaChannel(const String &value)
{
	String channel = value;
	channel.trim();
	channel.toLowerCase();
	return channel == "preview" ? "preview" : String(NOTIFICATOR_OTA_DEFAULT_CHANNEL);
}

/** Return true when value is exactly one lowercase SHA-256 hexadecimal digest. */
bool isSha256Hex(const String &value)
{
	if (value.length() != 64)
		return false;
	for (size_t i = 0; i < value.length(); ++i)
	{
		const char c = value[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}
	return true;
}

/** Encode a 32-byte digest as lowercase hexadecimal without heap allocation. */
String sha256ToHex(const unsigned char digest[32])
{
	static const char alphabet[] = "0123456789abcdef";
	char output[65];
	for (size_t i = 0; i < 32; ++i)
	{
		output[i * 2] = alphabet[(digest[i] >> 4) & 0x0f];
		output[i * 2 + 1] = alphabet[digest[i] & 0x0f];
	}
	output[64] = '\0';
	return String(output);
}
} // namespace

/**
 * Download and authenticate release metadata from the fixed official manifest.
 *
 * The manifest can be hosted publicly because authenticity comes from the
 * embedded public key. The corresponding private key never reaches a device.
 */
bool fetchOfficialOtaRelease(const String &requestedChannel, OtaRelease &release, String &error)
{
	error = "";
	const String channel = normalizeOtaChannel(requestedChannel);

	WiFiClientSecure client;
	client.setCACert(OTA_CA_CERT);
	client.setTimeout(12000);

	HTTPClient http;
	http.setConnectTimeout(10000);
	http.setTimeout(12000);
	if (!http.begin(client, NOTIFICATOR_OTA_MANIFEST_URL))
	{
		error = "manifest_begin";
		return false;
	}

	const int statusCode = http.GET();
	if (statusCode != HTTP_CODE_OK)
	{
		error = "manifest_http_" + String(statusCode);
		http.end();
		return false;
	}

	const String body = http.getString();
	http.end();
	if (!body.length() || body.length() > 12288)
	{
		error = "manifest_size";
		return false;
	}

	JsonDocument document;
	const DeserializationError jsonError = deserializeJson(document, body);
	if (jsonError)
	{
		error = "manifest_json";
		return false;
	}

	const int schemaVersion = document["schemaVersion"] | 0;
	JsonObject releaseJson =
		document["channels"][channel.c_str()]["deviceTypes"][NOTIFICATOR_OTA_DEVICE_TYPE].as<JsonObject>();
	if (schemaVersion != 2 || releaseJson.isNull())
	{
		error = "manifest_schema";
		return false;
	}

	release.channel = channel;
	release.deviceType = releaseJson["deviceType"] | "";
	release.board = releaseJson["board"] | "";
	release.version = releaseJson["version"] | "";
	release.url = releaseJson["url"] | "";
	release.sha256 = releaseJson["sha256"] | "";
	release.size = releaseJson["size"] | 0;
	release.releasedAt = releaseJson["releasedAt"] | "";
	release.signature = releaseJson["signature"] | "";
	release.keyId = releaseJson["keyId"] | "";

	release.deviceType.trim();
	release.board.trim();
	release.version.trim();
	release.url.trim();
	release.sha256.trim();
	release.sha256.toLowerCase();
	release.releasedAt.trim();
	release.signature.trim();
	release.keyId.trim();

	int major = 0;
	int minor = 0;
	int patch = 0;
	const String algorithm = releaseJson["signatureAlgorithm"] | "";
	if (release.deviceType != NOTIFICATOR_OTA_DEVICE_TYPE ||
		release.board != NOTIFICATOR_OTA_BOARD ||
		!parseVersionTriplet(release.version, major, minor, patch) ||
		!isValidOtaUrl(release.url) ||
		!isSha256Hex(release.sha256) ||
		release.size == 0 ||
		release.size > 1966080 ||
		!release.releasedAt.length() ||
		release.keyId != NOTIFICATOR_OTA_KEY_ID ||
		algorithm != "ECDSA-P256-SHA256")
	{
		error = "manifest_fields";
		return false;
	}

	const String signBase = buildOtaReleaseSignBase(
		release.channel,
		release.deviceType,
		release.board,
		release.version,
		release.url,
		release.sha256,
		release.size,
		release.releasedAt);
	if (!verifyOtaReleaseSignature(
			NOTIFICATOR_OTA_PUBLIC_KEY_PEM,
			signBase,
			release.signature))
	{
		error = "manifest_signature";
		return false;
	}

	return true;
}

/**
 * Stream an authenticated release into the inactive OTA slot.
 *
 * SHA-256 is calculated while writing, before Update.end() marks the slot as
 * bootable. This keeps memory use small and prevents a mismatched binary from
 * becoming the next boot image.
 */
bool streamVerifiedOtaImage(const OtaRelease &release, String &error)
{
	error = "";
	WiFiClientSecure client;
	client.setCACert(OTA_CA_CERT);
	client.setTimeout(12000);

	HTTPClient http;
	http.setConnectTimeout(10000);
	http.setTimeout(12000);
	if (!http.begin(client, release.url))
	{
		error = "binary_begin";
		return false;
	}

	const int statusCode = http.GET();
	if (statusCode != HTTP_CODE_OK)
	{
		error = "binary_http_" + String(statusCode);
		http.end();
		return false;
	}

	const int contentLength = http.getSize();
	if (contentLength > 0 && static_cast<size_t>(contentLength) != release.size)
	{
		error = "binary_size";
		http.end();
		return false;
	}

	if (!Update.begin(release.size, U_FLASH))
	{
		error = "update_begin_" + String(Update.getError());
		http.end();
		return false;
	}

	mbedtls_sha256_context shaContext;
	mbedtls_sha256_init(&shaContext);
	if (mbedtls_sha256_starts(&shaContext, 0) != 0)
	{
		error = "sha_begin";
		mbedtls_sha256_free(&shaContext);
		Update.abort();
		http.end();
		return false;
	}

	WiFiClient *stream = http.getStreamPtr();
	unsigned char buffer[1024];
	size_t received = 0;
	unsigned long lastDataAt = millis();
	unsigned long lastDrawAt = 0;

	while (received < release.size)
	{
		const int available = stream->available();
		if (available <= 0)
		{
			if (!stream->connected() || millis() - lastDataAt > 12000)
			{
				error = "binary_timeout";
				break;
			}
			delay(2);
			continue;
		}

		const size_t remaining = release.size - received;
		const size_t wanted = min(
			remaining,
			static_cast<size_t>(min(available, static_cast<int>(sizeof(buffer)))));
		const size_t count = stream->readBytes(buffer, wanted);
		if (!count)
			continue;

		lastDataAt = millis();
		if (mbedtls_sha256_update(&shaContext, buffer, count) != 0)
		{
			error = "sha_update";
			break;
		}
		if (Update.write(buffer, count) != count)
		{
			error = "update_write_" + String(Update.getError());
			break;
		}
		received += count;

		if (millis() - lastDrawAt >= 250)
		{
			lastDrawAt = millis();
			const int percent = static_cast<int>((received * 100ULL) / release.size);
			char percentText[8];
			snprintf(percentText, sizeof(percentText), "%d%%", percent);
			drawCenteredText("UPDATING", percentText, 2);
		}
	}

	unsigned char digest[32];
	const bool digestReady =
		!error.length() &&
		received == release.size &&
		mbedtls_sha256_finish(&shaContext, digest) == 0;
	mbedtls_sha256_free(&shaContext);

	if (!digestReady)
	{
		if (!error.length())
			error = "binary_incomplete";
		Update.abort();
		http.end();
		return false;
	}

	const String actualSha256 = sha256ToHex(digest);
	if (!otaSecureHexEquals(release.sha256, actualSha256))
	{
		error = "binary_hash";
		Update.abort();
		http.end();
		return false;
	}

	if (!Update.end() || !Update.isFinished())
	{
		error = "update_end_" + String(Update.getError());
		Update.abort();
		http.end();
		return false;
	}

	http.end();
	return true;
}

/** Check the official channel and install a newer authenticated release. */
void performOfficialOtaUpdate(const String &requestedChannel, bool force)
{
	const String channel = normalizeOtaChannel(requestedChannel);
	drawCenteredText("OTA", "CHECKING", 2);
	flashOnboardLedColor(60, 140, 255, 100);

	OtaRelease release;
	String error;
	if (!fetchOfficialOtaRelease(channel, release, error))
	{
		drawCenteredText("OTA CHECK", "FAILED", 2);
		publishDeviceStatus("ota_result", "failed", "", error);
		flashOnboardLedColor(255, 40, 40, 140);
		delay(1300);
		return;
	}

	if (!force && !isRemoteVersionNewer(String(FW_VERSION), release.version))
	{
		drawCenteredText("OTA", "UP TO DATE", 2);
		publishDeviceStatus("ota_result", "no_update", release.version, "");
		flashOnboardLedColor(0, 200, 140, 100);
		delay(1200);
		return;
	}

	drawCenteredText("OTA", "STARTING", 2);
	publishDeviceStatus("ota_result", "updating", release.version, "");
	delay(500);

	if (!streamVerifiedOtaImage(release, error))
	{
		drawCenteredText("OTA", "FAILED", 2);
		publishDeviceStatus("ota_result", "failed", release.version, error);
		flashOnboardLedColor(255, 40, 40, 150);
		delay(1400);
		return;
	}

	drawCenteredText("OTA OK", "RESTARTING", 2);
	publishDeviceStatus("ota_result", "success", release.version, "");
	flashOnboardLedColor(0, 200, 140, 160);
	delay(1400);
	ESP.restart();
}

/**
 * Parse and execute a command received on this device's MQTT command topic.
 *
 * OTA commands select only a release channel. Trust decisions remain on the
 * device and require an authenticated manifest plus a matching binary hash.
 */
void handleCmdJson(const String &json)
{
	StaticJsonDocument<512> doc;
	if (deserializeJson(doc, json) != DeserializationError::Ok)
		return;
	if (!doc.is<JsonObject>())
		return;

	const char *cmd = doc["cmd"] | "";
	int value = doc["value"] | -1;
	const char *otaChannel = doc["channel"] | NOTIFICATOR_OTA_DEFAULT_CHANNEL;
	bool forceOta = doc["force"] | false;

	// Supported commands:
	//  - idle_theme {value:0|1|2}
	//  - clear_msgs
	//  - mark_all_read
	//  - weather_config {lat?, lon?, city?, timezone?}
	//  - ota {channel:"stable"|"preview", force?:bool}

	if (strcmp(cmd, "idle_theme") == 0 && (value == 0 || value == 1 || value == 2))
	{
		uint8_t normalized = (uint8_t)value;
		saveIdleTheme(normalized);
		if (normalized == 0)
			drawCenteredText("IDLE", "CLOCK", 2);
		else if (normalized == 1)
			drawCenteredText("IDLE", "HYBRID", 2);
		else
			drawCenteredText("IDLE", "WEATHER", 2);
		flashOnboardLedColor(40, 120, 255, 40);
		flashOnboardLedColor(40, 120, 255, 40);
		lastUserOrMsgMs = millis() - (IDLE_AFTER_MS + 1000);
		noIdleUntilMs = millis();
		return;
	}

	if (strcmp(cmd, "clear_msgs") == 0)
	{
		clearAllMessagesAndShowFeedback();
		return;
	}

	if (strcmp(cmd, "mark_all_read") == 0)
	{
		markAllReadAndPersist();
		drawCenteredText("MARKED", "READ", 2);
		flashOnboardLedColor(0, 180, 40, 40);
		flashOnboardLedColor(0, 180, 40, 40);
		bumpNoIdleGuard();
		return;
	}

	if (strcmp(cmd, "weather_config") == 0)
	{
		bool hasLat = !doc["lat"].isNull() || !doc["latitude"].isNull();
		bool hasLon = !doc["lon"].isNull() || !doc["longitude"].isNull();

		if (hasLat != hasLon)
		{
			drawCenteredText("WEATHER", "BAD COORD", 2);
			flashOnboardLedColor(255, 120, 0, 120);
			delay(900);
			return;
		}

		float lat = geoLat;
		float lon = geoLon;
		if (hasLat && hasLon)
		{
			lat = !doc["lat"].isNull() ? doc["lat"].as<float>() : doc["latitude"].as<float>();
			lon = !doc["lon"].isNull() ? doc["lon"].as<float>() : doc["longitude"].as<float>();

			if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f)
			{
				drawCenteredText("WEATHER", "BAD RANGE", 2);
				flashOnboardLedColor(255, 120, 0, 120);
				delay(900);
				return;
			}
		}

		String city = "";
		if (!doc["city"].isNull())
			city = String((const char *)doc["city"]);
		if (!city.length() && !doc["location"].isNull())
			city = String((const char *)doc["location"]);
		city.trim();

		String tz = "";
		if (!doc["timezone"].isNull())
			tz = String((const char *)doc["timezone"]);
		if (!tz.length() && !doc["tz"].isNull())
			tz = String((const char *)doc["tz"]);
		tz.trim();

		geoLat = lat;
		geoLon = lon;

		if (city.length())
		{
			geoCity = city;
			geoCity.toUpperCase();
			if (geoCity.length() > 12)
				geoCity = geoCity.substring(0, 12);
		}
		else if (!geoCity.length())
		{
			geoCity = "GEO";
		}

		if (tz.length())
		{
			geoTz = tz;
		}
		else if (!geoTz.length())
		{
			geoTz = "Europe/Athens";
		}
		applyDeviceTimezone();

		// Force NTP to rebind to the active timezone and refresh quickly.
		if (WiFi.isConnected())
		{
			configTzTime(resolveTimezonePosix(), "pool.ntp.org", "time.nist.gov");
			ntpStarted = true;
		}

		geoManualOverride = true;
		geoHasData = true;
		geoFetching = false;
		lastGeoFetchMs = millis();

		// Force immediate weather refresh using the new location.
		lastWeatherFetchMs = 0;
		weatherHasData = false;
		weatherFetching = false;

		saveWeatherConfig(true);
		return;
	}

	if (strcmp(cmd, "ota") == 0)
	{
		performOfficialOtaUpdate(String(otaChannel), forceOta);
		return;
	}
}

// -------------------- MQTT --------------------
/**
 * Configure the broker endpoint and normalize inbound MQTT payloads.
 *
 * Command messages are routed to handleCmdJson(). Notification messages accept
 * either the structured JSON contract or the legacy pipe-delimited format.
 */
void setupMqttClient()
{
	if (!mqttConfigValid)
		return;

	mqttClient.setServer(mqttHost.c_str(), mqttPort);
	mqttClient.setBufferSize(1024);

	mqttClient.setCallback([](char *topic, uint8_t *payload, unsigned int length)
						   {
		String receivedTopic = String(topic ? topic : "");

		String raw;
		raw.reserve(length + 1);
		for (unsigned int index = 0; index < length; index++)
			raw += static_cast<char>(payload[index]);

		if (mqttCmdTopic.length() && receivedTopic == mqttCmdTopic)
		{
			handleCmdJson(raw);
			return;
		}

		receivingUntilMs = millis() + RECEIVING_BADGE_MS;

		String title;
		String body;
		String type;
		String severity;

		StaticJsonDocument<1024> doc;
		DeserializationError error = deserializeJson(doc, raw);

		if (!error && doc.is<JsonObject>())
		{
			JsonObject object = doc.as<JsonObject>();
			if (object["title"].is<const char *>())
				title = String(object["title"].as<const char *>());
			if (object["body"].is<const char *>())
				body = String(object["body"].as<const char *>());
			if (object["type"].is<const char *>())
				type = String(object["type"].as<const char *>());
			if (object["severity"].is<const char *>())
				severity = String(object["severity"].as<const char *>());
		}
		else
		{
			const int firstSeparator = raw.indexOf('|');
			if (firstSeparator >= 0)
			{
				title = raw.substring(0, firstSeparator);
				String remainder = raw.substring(firstSeparator + 1);
				const int secondSeparator = remainder.indexOf('|');
				if (secondSeparator >= 0)
				{
					body = remainder.substring(0, secondSeparator);
					severity = remainder.substring(secondSeparator + 1);
				}
				else
				{
					body = remainder;
				}
			}
			else
			{
				body = raw;
			}
		}

		String payloadText;
		payloadText.reserve(160);
		if (title.length() && body.length())
			payloadText = title + "|" + body;
		else if (title.length())
			payloadText = title;
		else
			payloadText = body;
		if (!payloadText.length())
			payloadText = "(empty)";

		const String header = makeHeaderWithType(severity.length() ? severity : type);
		pushMessage(header, payloadText);
		showCurrentMessage(false);

		const unsigned long now = millis();
		if (now - lastMsgLedFlashMs >= MSG_LED_FLASH_COOLDOWN_MS)
		{
			flashOnboardLedColor(0, 160, 255, 80);
			lastMsgLedFlashMs = now;
		} });

	if (MQTT_USE_TLS && MQTT_CA_CERT[0] != '\0')
	{
		tlsClient.setCACert(MQTT_CA_CERT);
	}
}

/**
 * Attempt one authenticated MQTT connection and restore subscriptions.
 *
 * Retry cadence is controlled by loop(), so this function performs no delay
 * and returns immediately when Wi-Fi or broker configuration is unavailable.
 */
void connectToMqtt()
{
	if (mqttClient.connected())
		return;
	if (!WiFi.isConnected())
		return;
	if (!mqttConfigValid)
		return;

	const String clientId = String("notificator-") + deviceId;
	bool connected = mqttClient.connect(clientId.c_str(), mqttUsername.c_str(), mqttPassword.c_str());

	if (connected)
	{
		if (mqttSubTopic.length())
			mqttClient.subscribe(mqttSubTopic.c_str(), 1);
		if (mqttCmdTopic.length())
			mqttClient.subscribe(mqttCmdTopic.c_str(), 1);
		publishDeviceStatus("online", "ready", "", "");
		flashOnboardLedColor(0, 220, 160, 70);
	}
}

// -------------------- Capacitive touch handling --------------------
static bool readTtpRawPressed()
{
	int ttpV = digitalRead(TTP223_PIN);
	return (ttpV != (int)ttpIdleLevel);
}

/**
 * Sample the TTP223 and resolve tap/hold gestures without blocking the network
 * loop. Gesture actions run only after a stable release.
 */
void handleTouchInput()
{
	// The TTP223 is the only control on current devices. Actions are resolved
	// after release so a tap cannot accidentally become a destructive hold.
	unsigned long now = millis();
	bool rawTtp = readTtpRawPressed();

	if (rawTtp != ttpRawLast)
	{
		ttpRawLast = rawTtp;
		ttpRawChangedMs = now;
	}
	if ((now - ttpRawChangedMs) >= BUTTON_DEBOUNCE_MS)
	{
		ttpStable = rawTtp;
	}

	if (!touchDown && ttpStable)
	{
		touchDown = true;
		touchDownMs = now;
		holdPreviewActive = false;
		holdPreviewMs = 0;
		return;
	}

	if (touchDown)
	{
		if (ttpStable)
		{
			unsigned long held = now - touchDownMs;
			holdPreviewMs = held;
			holdPreviewActive = (held >= HOLD_PREVIEW_START_MS);
			return;
		}

		touchDown = false;
		unsigned long held = now - touchDownMs;
		holdPreviewActive = false;
		holdPreviewMs = 0;

		// Ignore very short pulses, common with capacitive sensor jitter.
		if (held < TAP_MIN_PRESS_MS)
		{
			return;
		}

		if (held >= BUTTON_SETUP_LONG_PRESS_MS)
		{
			resetTapSequence();
			if (!portalRunning)
				startSetupPortal();
			bumpNoIdleGuard();
			return;
		}

		if (held >= BUTTON_LONG_PRESS_MS)
		{
			resetTapSequence();
			markAllReadAndPersist();
			return;
		}

		if (!tapPending)
		{
			tapSequenceStartedFromIdle =
				(now - lastUserOrMsgMs) > IDLE_AFTER_MS &&
				unreadCount() == 0 &&
				(long)(now - noIdleUntilMs) >= 0;
		}
		tapCount++;
		tapPending = true;
		tapDeadlineMs = now + TAP_WINDOW_MS;
		bumpNoIdleGuard();
		return;
	}

	if (tapPending && (long)(now - tapDeadlineMs) >= 0)
	{
		uint8_t c = tapCount;
		tapCount = 0;
		tapPending = false;
		const bool startedFromIdle = tapSequenceStartedFromIdle;
		tapSequenceStartedFromIdle = false;

		// Gesture map (normal mode):
		// 1 tap = wake or next message, 2 taps = toggle read/unread,
		// 3 taps = device and connection info.
		if (c >= SHOW_ID_TAP_COUNT)
		{
			showIdSticky = true;
			showIdUntilMs = 0;
			bumpNoIdleGuard();
			return;
		}
		if (c == 1)
		{
			// If info overlay is visible, close it and allow idle immediately when there are no unread messages.
			if (showIdSticky || showIdUntilMs > now)
			{
				showIdSticky = false;
				showIdUntilMs = 0;
				if (unreadCount() == 0)
				{
					lastUserOrMsgMs = millis() - (IDLE_AFTER_MS + 1000);
					noIdleUntilMs = millis();
				}
				else
				{
					bumpNoIdleGuard();
				}
				return;
			}

			if (!hasMessages())
			{
				showNoMessagesOverlay();
				return;
			}

			if (startedFromIdle)
				showCurrentMessage(false);
			else
				gotoNextMessage(false);
			return;
		}
		if (c == 2)
		{
			toggleCurrentReadStateAndPersist();
			return;
		}
	}
}

// -------------------- Idle clock (theme 0) --------------------
void drawIdleClockFrame()
{
	unsigned long now = millis();
	if (now - lastClockDrawMs < CLOCK_REDRAW_MS)
		return;
	lastClockDrawMs = now;

	display.clearDisplay();
	drawStatusBar(true);

	String hhmm = humanTimeHHMM();

	int16_t x1, y1;
	uint16_t w, h;

	display.setTextColor(SSD1306_WHITE);
	display.setTextSize(4);
	display.getTextBounds(hhmm.c_str(), 0, 0, &x1, &y1, &w, &h);
	int x = (OLED_WIDTH - (int)w) / 2;
	int y = 32 - (int)h / 2;
	if (y < 18)
		y = 18;
	display.setCursor(x, y);
	display.print(hhmm);

	display.setTextSize(2);
	if (timeReady())
	{
		time_t t = time(nullptr);
		struct tm tm;
		localtime_r(&t, &tm);
		char d[12];
		snprintf(d, sizeof(d), "%02d/%02d/%04d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
		display.getTextBounds(d, 0, 0, &x1, &y1, &w, &h);
		display.setCursor((OLED_WIDTH - (int)w) / 2, 48);
		display.print(d);
	}
	else
	{
		const char *msg = "SYNC TIME";
		display.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
		display.setCursor((OLED_WIDTH - (int)w) / 2, 48);
		display.print(msg);
	}

	display.display();
}

void drawIdleHybridFrame()
{
	// Alternate in 2.5s phases between clock and weather idle UIs.
	unsigned long phase = (millis() / IDLE_HYBRID_PHASE_MS) % 2;
	if (phase == 0)
		drawIdleClockFrame();
	else
		drawIdleWeatherFrame();
}

// -------------------- Setup --------------------
/**
 * Initialize hardware, restore persistent state, and select setup or normal
 * operation. Blocking work is limited to short boot feedback and fatal display
 * initialization failure.
 */
void setup()
{
	Serial.begin(115200);
	randomSeed((uint32_t)esp_random());

	// Default to Greece timezone (POSIX TZ rules for ESP32)
	setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
	tzset();

	setOnboardLed(false);

	pinMode(TTP223_PIN, INPUT);
	delay(5);
	ttpIdleLevel = (uint8_t)digitalRead(TTP223_PIN);

	ttpRawLast = readTtpRawPressed();
	ttpStable = ttpRawLast;
	ttpRawChangedMs = millis();

	touchDown = false;
	tapCount = 0;
	tapPending = false;

	Wire.begin(I2C_SDA, I2C_SCL);
	if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
	{
		while (true)
			delay(1000);
	}

	drawBootWelcomeScreen();
	delay(1200);

	loadConfiguredFlag();
	loadIdleTheme();
	loadWeatherConfig();

	deviceId = String((uint32_t)ESP.getEfuseMac(), HEX);
	apSsid = String(WIFI_AP_PREFIX) + deviceId;
	loadMqttConfig();
	rebuildMqttTopics();

	loadHistoryFromPrefs();
	focusFirstUnreadIfAny();

	configureSetupPortal();
	{
		std::vector<const char *> menu = {"custom"};
		wm.setMenu(menu);
	}

	WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
				 {
    if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
      portalClientConnected = true;
      setupScreenDrawn = false;
      flashOnboardLedColor(170, 60, 255, 50);
      return;
    }
    if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) { portalClientConnected = false; setupScreenDrawn = false; return; }

    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      wifiConnectedAtMs = millis();
      wifiConnectingSinceMs = 0;
      lastWifiAttemptMs = 0;
      wifiHardResetDone = false;
      flashOnboardLedColor(0, 220, 70, 70);

      if (!ntpStarted) {
        const char *tzPosix = resolveTimezonePosix();
        configTzTime(tzPosix, "pool.ntp.org", "time.nist.gov");
        ntpStarted = true;
      }
      return;
    }

    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      (void)info;
      if (wifiConnectingSinceMs == 0) wifiConnectingSinceMs = millis();
      flashOnboardLedColor(255, 40, 40, 50);
      return;
    } });

	setupMqttClient();

	lastClockDrawMs = 0;

	// BOOT: if no unread, make idle eligible immediately
	bumpNoIdleGuard();
	if (unreadCount() == 0)
	{
		lastUserOrMsgMs = millis() - (IDLE_AFTER_MS + 1000);
		noIdleUntilMs = millis(); // allow immediately
	}

	if (deviceConfigured && mqttConfigValid)
	{
		portalRunning = false;
		drawCenteredText("STARTING", "DEVICE", 2);
		startStaConnectStable();
	}
	else
	{
		startSetupPortal();
	}
}

// -------------------- Loop --------------------
/**
 * Run the cooperative device state machine.
 *
 * Evaluation order:
 * 1. Process local input and deferred history saves.
 * 2. Refresh time and weather data while Wi-Fi is available.
 * 3. Service or finalize the setup portal.
 * 4. Recover Wi-Fi and maintain the MQTT session.
 * 5. Render the highest-priority OLED state.
 *
 * Early returns intentionally prevent setup, recovery, gesture feedback, and
 * notification rendering from drawing over one another.
 */
void loop()
{
	unsigned long now = millis();

	handleTouchInput();
	maybeSaveHistory();

	if (WiFi.isConnected())
	{
		restampTimePlaceholdersIfReady();
		// geo + weather happen when idleTheme is hybrid/weather (inside maybeFetchWeather)
		maybeFetchWeather();
	}

	// Keep hold feedback visible and stable while the user is pressing.
	if (holdPreviewActive && touchDown)
	{
		drawHoldCounter(holdPreviewMs);
		return;
	}

	// ---------- Portal mode ----------
	if (portalRunning && !WiFi.isConnected())
	{
		wm.process();

		if (!portalStartChecked && (millis() - portalStartMs) > 1200)
		{
			portalStartChecked = true;
			IPAddress ip = WiFi.softAPIP();
			bool apLooksUp = (ip[0] != 0 || ip[1] != 0 || ip[2] != 0 || ip[3] != 0);
			if (!apLooksUp)
			{
				drawStatus("WiFi", "AP failed");
				startSetupPortal();
			}
		}

		if (portalClientConnected)
		{
			drawPortalAnimationFrame();
			setupScreenDrawn = false;
		}
		else
		{
			drawSetupInstructions();
			setupScreenDrawn = true;
		}
		return;
	}

	// ---------- If portal was running and STA connected ----------
	if (portalRunning && WiFi.isConnected())
	{
		portalRunning = false;
		finalizeSetupAfterPortal();
		return;
	}

	// ---------- CLEAN WiFi recovery ----------
	if (deviceConfigured && !WiFi.isConnected())
	{
		if (wifiConnectingSinceMs == 0)
			wifiConnectingSinceMs = now;

		if (now - lastWifiDrawMs >= WIFI_DRAW_MS)
		{
			lastWifiDrawMs = now;
			if ((now / 600) % 2 == 0)
				drawCenteredText("CONNECTING", "WIFI", 2);
			else
				drawCenteredText("CONNECTING", "WIFI..", 2);
		}

		if (wifiConnectedAtMs != 0 && (now - wifiConnectedAtMs) < WIFI_POST_CONNECT_GRACE_MS)
			return;

		if (now - lastWifiAttemptMs >= WIFI_RETRY_MS)
		{
			lastWifiAttemptMs = now;
			WiFi.reconnect();
		}

		if (!wifiHardResetDone && (now - wifiConnectingSinceMs) >= WIFI_HARD_RESET_AFTER_MS)
		{
			hardResetWiFiStackOnce();
		}

		if ((now - wifiConnectingSinceMs) >= WIFI_PORTAL_AFTER_MS)
		{
			startSetupPortal();
		}

		return;
	}

	// ---------- MQTT ----------
	if (!WiFi.isConnected())
		return;

	if (!mqttClient.connected())
	{
		if (now - lastMqttAttemptMs >= MQTT_RECONNECT_MS)
		{
			lastMqttAttemptMs = now;
			connectToMqtt();
		}
	}
	else
	{
		mqttClient.loop();

	}

	// ---------- Screen refresh (NO JUMPS + IDLE FIX) ----------
	static unsigned long lastDrawMs = 0;
	if (now - lastDrawMs >= 700)
	{
		lastDrawMs = now;

		if (showIdSticky || showIdUntilMs > now)
		{
			drawDeviceIdScreen();
			return;
		}

		if (showNoMessagesUntilMs > now)
		{
			drawNoMessagesScreen();
			return;
		}

		if (uiFeedbackUntilMs > now)
		{
			drawGestureFeedback();
			return;
		}
		uiFeedback = UiFeedback::None;

		const bool idleTimeReached = (now - lastUserOrMsgMs) > IDLE_AFTER_MS;
		const bool noUnread = (unreadCount() == 0);
		const bool idleAllowed = (long)(now - noIdleUntilMs) >= 0;

		if (idleAllowed && idleTimeReached && noUnread)
		{
			if (idleTheme == 0)
				drawIdleClockFrame();
			else if (idleTheme == 1)
				drawIdleHybridFrame();
			else if (idleTheme == 2)
				drawIdleWeatherFrame();
			else
				drawIdleClockFrame();
			return;
		}

		if (hasMessages())
		{
			bool curUnread = !messageBuffer[currentIndex].read;
			drawWrappedMessage(messageBuffer[currentIndex].topic, messageBuffer[currentIndex].payload, curUnread);
		}
		else
		{
			// no messages but not allowed to idle yet => status bar only
			display.clearDisplay();
			drawStatusBar(false);
			display.display();
		}
	}
}
