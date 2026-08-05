#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <lvgl.h>
#include <math.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"

#include "src/axs15231b/esp_lcd_axs15231b.h"
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "portal_ui.h"
#include "ota_release_config.h"
#include "ota_security.h"

/**
 * @file notificator_touch_349.ino
 * @brief Production firmware entry point for Notificator Touch 3.49.
 *
 * This first production slice owns the validated display, touch, orientation,
 * audio, and physical-button layer plus the model-specific Notificator UI.
 * Network provisioning and MQTT delivery are isolated from the rendering and
 * hardware layers so credentials and transport state remain easy to audit.
 */

namespace
{
constexpr char FIRMWARE_VERSION[] = "0.9.1";
constexpr char FIRMWARE_LABEL[] = "0.9.1 PREVIEW";
constexpr char MODEL_ID[] = "notificator_touch_349";
constexpr char WIFI_AP_PREFIX[] = "WPNOTIF-";
constexpr char DEFAULT_MQTT_TOPIC_PREFIX[] = "notificator-project";
constexpr uint16_t DEFAULT_MQTT_PORT = 8883;
constexpr unsigned long MQTT_RECONNECT_MS = 5000;
constexpr unsigned long MQTT_STATUS_INTERVAL_MS = 60000;
constexpr unsigned long MQTT_HEALTH_GRACE_MS = 5000;
constexpr unsigned long MQTT_PULSE_INTERVAL_MS = 1000;
constexpr unsigned long BATTERY_SAMPLE_INTERVAL_MS = 30000;
constexpr unsigned long WEATHER_REFRESH_INTERVAL_MS = 15UL * 60UL * 1000UL;
constexpr unsigned long IDLE_CLOCK_TIMEOUT_MS = 60000;
constexpr unsigned long SETUP_BUTTON_HOLD_MS = 4000;
constexpr unsigned long WIFI_WIZARD_TIMEOUT_MS = 18000;
constexpr uint8_t MAX_WIFI_SCAN_RESULTS = 12;
constexpr uint8_t DEFAULT_SCREEN_BRIGHTNESS = 80;
constexpr uint8_t DEFAULT_SOUND_VOLUME = 68;
constexpr uint8_t SETTINGS_STEP = 10;

constexpr int LCD_NATIVE_WIDTH = 172;
constexpr int LCD_NATIVE_HEIGHT = 640;
constexpr int SCREEN_WIDTH = 640;
constexpr int SCREEN_HEIGHT = 172;
constexpr int LCD_CHUNK_HEIGHT = 32;

constexpr gpio_num_t LCD_PIN_CS = GPIO_NUM_9;
constexpr gpio_num_t LCD_PIN_CLOCK = GPIO_NUM_10;
constexpr gpio_num_t LCD_PIN_DATA_0 = GPIO_NUM_11;
constexpr gpio_num_t LCD_PIN_DATA_1 = GPIO_NUM_12;
constexpr gpio_num_t LCD_PIN_DATA_2 = GPIO_NUM_13;
constexpr gpio_num_t LCD_PIN_DATA_3 = GPIO_NUM_14;
constexpr gpio_num_t LCD_PIN_RESET = GPIO_NUM_21;
constexpr gpio_num_t LCD_PIN_BACKLIGHT = GPIO_NUM_8;

constexpr int TOUCH_PIN_SDA = 17;
constexpr int TOUCH_PIN_SCL = 18;
constexpr uint8_t TOUCH_ADDRESS = 0x3B;
constexpr uint8_t BATTERY_ADC_PIN = 4;
constexpr float BATTERY_DIVIDER_RATIO = 3.0F;

constexpr int SYSTEM_PIN_SDA = 47;
constexpr int SYSTEM_PIN_SCL = 48;
constexpr uint8_t RTC_ADDRESS = 0x51;
constexpr uint8_t IMU_ADDRESS = 0x6B;
constexpr uint8_t AUDIO_CODEC_ADDRESS = 0x18;
constexpr uint8_t IO_EXPANDER_ADDRESS = 0x20;

constexpr int SWIPE_MIN_DISTANCE = 72;
constexpr int AUDIO_SAMPLE_RATE = 24000;
constexpr int BUTTON_PIN_BOOT = 0;
constexpr int BUTTON_PIN_POWER = 16;
constexpr unsigned long BUTTON_DEBOUNCE_MS = 28;
constexpr unsigned long POWER_OFF_HOLD_MS = 1800;
constexpr uint8_t IMU_REGISTER_CTRL1 = 0x02;
constexpr uint8_t IMU_REGISTER_CTRL2 = 0x03;
constexpr uint8_t IMU_REGISTER_CTRL5 = 0x06;
constexpr uint8_t IMU_REGISTER_CTRL7 = 0x08;
constexpr uint8_t IMU_REGISTER_ACCEL_X_LOW = 0x35;
constexpr uint8_t IMU_REGISTER_RESET_RESULT = 0x4D;
constexpr uint8_t IMU_REGISTER_RESET = 0x60;
constexpr int IMU_FLIP_THRESHOLD = 4300;
constexpr unsigned long IMU_FLIP_STABILITY_MS = 220;

// RGB565 equivalents of the WordPress plugin's dark navy and action-blue
// palette. The shared colors make Touch feel like the physical extension of
// the dashboard rather than a separate interface.
constexpr uint16_t COLOR_BACKGROUND = 0x0083;   // #07101f
constexpr uint16_t COLOR_PANEL = 0x08C5;        // #0f1b2d
constexpr uint16_t COLOR_PANEL_RAISED = 0x1128; // #172742
constexpr uint16_t COLOR_BLUE = 0x231D;         // #2563eb
constexpr uint16_t COLOR_BLUE_LIGHT = 0x653F;   // #60a5fa
constexpr uint16_t COLOR_GREEN = 0x262B;        // #22c55e
constexpr uint16_t COLOR_AMBER = 0xF4E1;        // #f59e0b
constexpr uint16_t COLOR_CYAN = COLOR_BLUE_LIGHT;
constexpr uint16_t COLOR_RED = 0xEA28;   // #ef4444
constexpr uint16_t COLOR_MUTED = 0x9517; // #94a3b8
constexpr uint16_t COLOR_WHITE = 0xFFDF; // #f8fafc
constexpr uint16_t COLOR_BLACK = 0x0000;

// HiveMQ Cloud certificates chain to ISRG Root X1. Keeping the trust anchor in
// firmware provides authenticated TLS without storing any user credentials in
// the source tree or in Notificator's API.
constexpr char MQTT_CA_CERT[] = R"CERT(
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
)CERT";

TwoWire touchWire(1);
esp_lcd_panel_handle_t lcdPanel = nullptr;
SemaphoreHandle_t transferComplete = nullptr;
uint16_t *transferBuffer = nullptr;
uint16_t *frameBuffer = nullptr;

bool displayReady = false;
bool psramReady = false;
bool rtcFound = false;
bool imuFound = false;
bool touchControllerFound = false;
bool audioReady = false;
bool imuRotationReady = false;
bool orientationCalibrated = false;
bool displayFlipped = false;
int wifiNetworkCount = -1;
unsigned long lastTouchLogMs = 0;
unsigned long lastImuSampleMs = 0;
unsigned long orientationCandidateSinceMs = 0;
esp_codec_dev_handle_t audioPlayback = nullptr;
uint8_t currentPage = 0;
bool touchActive = false;
uint8_t missedTouchReads = 0;
uint16_t touchStartX = 0;
uint16_t touchStartY = 0;
uint16_t touchLastX = 0;
uint16_t touchLastY = 0;
int8_t orientationAxis = 1;
int8_t orientationReferenceSign = 1;
bool orientationCandidate = false;
bool deferPagePresentation = false;
bool portalRunning = false;
bool portalSaveRequested = false;
bool mqttConfigValid = false;
bool deviceConfigured = false;
bool networkStateChanged = false;
bool idleClockActive = false;
bool lvglReady = false;
bool clockSyncStarted = false;
bool batteryAvailable = false;
unsigned long lastMqttAttemptMs = 0;
unsigned long lastMqttStatusMs = 0;
unsigned long lastMqttHealthMs = 0;
unsigned long lastMqttPulseRenderMs = 0;
unsigned long lastBatterySampleMs = 0;
unsigned long lastInteractionMs = 0;
int lastClockMinute = -1;
int lastClockSecond = -1;
float batteryVoltage = 0.0F;
uint8_t batteryPercent = 0;
uint8_t screenBrightnessPercent = DEFAULT_SCREEN_BRIGHTNESS;
uint8_t soundVolumePercent = DEFAULT_SOUND_VOLUME;
int16_t clockUtcOffsetMinutes = 0;
uint8_t idleTheme = 0;
String weatherCity = "Athens";
String weatherTimezone;
float weatherLatitude = 0.0F;
float weatherLongitude = 0.0F;
bool weatherHasCoordinates = false;
bool weatherHasData = false;
bool weatherFetchInProgress = false;
float weatherTemperatureC = 0.0F;
float weatherWindKmh = 0.0F;
uint8_t weatherCode = 0;
unsigned long lastWeatherFetchMs = 0;

Preferences preferences;
WiFiManager wifiManager;
WiFiClientSecure mqttTlsClient;
PubSubClient mqttClient(mqttTlsClient);

lv_display_t *lvglDisplay = nullptr;
lv_indev_t *lvglTouchInput = nullptr;
lv_obj_t *lvglPageRoot = nullptr;
lv_obj_t *lvglSetupStatus = nullptr;
lv_obj_t *wifiPasswordInput = nullptr;

enum class WifiWizardStage : uint8_t
{
	Idle,
	Scanning,
	Results,
	Password,
	Connecting,
	Success,
	Failed,
};

WifiWizardStage wifiWizardStage = WifiWizardStage::Idle;
bool wifiWizardActive = false;
bool pendingWifiOpen = false;
unsigned long wifiConnectStartedMs = 0;
uint8_t wifiScanCount = 0;
String wifiScanSsids[MAX_WIFI_SCAN_RESULTS];
int32_t wifiScanRssi[MAX_WIFI_SCAN_RESULTS] = {};
bool wifiScanSecure[MAX_WIFI_SCAN_RESULTS] = {};
String pendingWifiSsid;
String pendingWifiPassword;
String wifiPasswordDraft;
String previousWifiSsid;
String previousWifiPassword;
uint8_t wifiKeyboardMode = 0;

String setupSsid;
String portalHeadHtml;
String mqttHost;
String mqttUsername;
String mqttPassword;
String mqttTopicPrefix = DEFAULT_MQTT_TOPIC_PREFIX;
String mqttMessageTopic;
String mqttCommandTopic;
String mqttStatusTopic;
String mqttLegacyMessageTopic;
String mqttLegacyCommandTopic;
uint16_t mqttPort = DEFAULT_MQTT_PORT;

char mqttHostField[97] = "";
char mqttPortField[6] = "8883";
char mqttUsernameField[65] = "";
char mqttPasswordField[97] = "";
char mqttTopicField[97] = "notificator-project";
char clockOffsetField[7] = "0";

String deviceIdentityHtml;
WiFiManagerParameter *deviceIdentityParameter = nullptr;
WiFiManagerParameter mqttSectionParameter(
	"<section class=\"mqtt-card\"><span class=\"provider\">HIVEMQ CLOUD</span><h2>Notification connection</h2>"
	"<p>Paste the credentials from your free HiveMQ Cloud cluster. They remain on this device.</p>");
WiFiManagerParameter mqttHostParameter("mqtt_host", "Cluster hostname", mqttHostField, sizeof(mqttHostField),
	"required maxlength=\"96\" autocapitalize=\"none\" autocomplete=\"off\" placeholder=\"abc123.s1.eu.hivemq.cloud\"");
WiFiManagerParameter mqttPortParameter("mqtt_port", "Secure port", mqttPortField, sizeof(mqttPortField),
	"required type=\"number\" min=\"1\" max=\"65535\" inputmode=\"numeric\"");
WiFiManagerParameter mqttUsernameParameter("mqtt_user", "Username", mqttUsernameField, sizeof(mqttUsernameField),
	"required maxlength=\"64\" autocapitalize=\"none\" autocomplete=\"username\"");
WiFiManagerParameter mqttPasswordParameter("mqtt_pass", "Password", mqttPasswordField, sizeof(mqttPasswordField),
	"maxlength=\"96\" type=\"password\" autocomplete=\"new-password\" placeholder=\"Leave blank to keep saved password\"");
WiFiManagerParameter mqttTopicParameter("mqtt_topic", "Topic prefix", mqttTopicField, sizeof(mqttTopicField),
	"required maxlength=\"96\" autocapitalize=\"none\" autocomplete=\"off\"");
WiFiManagerParameter clockOffsetParameter("clock_offset", "Clock UTC offset (minutes)", clockOffsetField,
	sizeof(clockOffsetField), "required type=\"number\" min=\"-720\" max=\"840\" step=\"15\" inputmode=\"numeric\"");
WiFiManagerParameter mqttSectionEndParameter("</section>");

struct ButtonState
{
	int pin;
	const char *name;
	bool rawPressed;
	bool pressed;
	unsigned long changedAtMs;
	unsigned long pressedAtMs;
	uint16_t pressCount;
	bool longActionHandled;
};

ButtonState buttons[] = {
	{BUTTON_PIN_BOOT, "BOOT", false, false, 0, 0, 0, false},
	{BUTTON_PIN_POWER, "POWER", false, false, 0, 0, 0, false},
};
String buttonPageMessage = "RESET RESTARTS IMMEDIATELY";

struct NotificationPreview
{
	String title;
	String body;
	String source;
	String receivedAt;
	String severity;
	bool available;
	bool unread;
};

struct OtaRelease
{
	String channel;
	String deviceType;
	String board;
	String version;
	String url;
	String sha256;
	size_t size = 0;
	String releasedAt;
	String signature;
	String keyId;
};

constexpr uint8_t MAX_ALERT_HISTORY = 6;
NotificationPreview alertHistory[MAX_ALERT_HISTORY] = {};
uint8_t alertCount = 0;
uint8_t selectedAlertIndex = 0;
String deviceId;

NotificationPreview *selectedAlert()
{
	return alertCount == 0 ? nullptr : &alertHistory[min<uint8_t>(selectedAlertIndex, alertCount - 1)];
}

uint8_t unreadAlertCount()
{
	uint8_t count = 0;
	for (uint8_t index = 0; index < alertCount; ++index)
		count += alertHistory[index].unread ? 1 : 0;
	return count;
}

void addAlert(String title, String body, String source, String severity = "info")
{
	title.toUpperCase();
	body.toUpperCase();
	source.toUpperCase();
	title = title.substring(0, 30);
	body = body.substring(0, 52);
	source = source.substring(0, 30);
	severity.toLowerCase();
	if (severity == "warning" || severity == "warn")
		severity = "warning";
	else if (severity == "critical" || severity == "error" || severity == "danger")
		severity = "critical";
	else
		severity = "info";
	String receivedAt = "JUST NOW";
	const time_t receivedTime = time(nullptr);
	if (receivedTime >= 1700000000)
	{
		tm localTime = {};
		localtime_r(&receivedTime, &localTime);
		char timeBuffer[6] = {};
		strftime(timeBuffer, sizeof(timeBuffer), "%H:%M", &localTime);
		receivedAt = timeBuffer;
	}

	const uint8_t lastIndex = min<uint8_t>(alertCount, MAX_ALERT_HISTORY - 1);
	for (int index = lastIndex; index > 0; --index)
		alertHistory[index] = alertHistory[index - 1];
	alertHistory[0] = {title, body, source, receivedAt, severity, true, true};
	alertCount = min<uint8_t>(alertCount + 1, MAX_ALERT_HISTORY);
	selectedAlertIndex = 0;
}

constexpr uint8_t DIGIT_GLYPHS[10][5] = {
	{0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
	{0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
	{0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
	{0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
	{0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
};

constexpr uint8_t LETTER_GLYPHS[26][5] = {
	{0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
	{0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
	{0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
	{0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
	{0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
	{0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
	{0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
	{0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
	{0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
	{0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
	{0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
	{0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
	{0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

void drawCurrentPage();
void drawSetupScreen(const String &state);
bool playTestChime();
void startSetupPortal();
void drawIdleClockScreen();
bool readTouch(uint16_t &screenX, uint16_t &screenY, uint8_t &points);
void initializeLvglUi();
void renderLvglPage(uint8_t page);
void renderLvglSetup(const String &state);
void renderLvglClock();
void handleDeviceCommand(const String &json);
void maybeRefreshWeather();
String weatherConditionLabel(uint8_t code);
void performOfficialOtaUpdate(const String &requestedChannel, bool force);
String displayPairingId();
bool readLocalClock(tm &clockTime);
void drawText(int x, int y, const String &text, int scale, uint16_t foreground, uint16_t background);
void drawCenteredText(int centerX, int y, const String &text, int scale,
	uint16_t foreground, uint16_t background);
void applyScreenBrightness();
void applySoundVolume();
void saveDisplayConfiguration();

uint16_t toPanelColor(uint16_t color)
{
	return static_cast<uint16_t>((color << 8) | (color >> 8));
}

bool IRAM_ATTR onColorTransferComplete(
	esp_lcd_panel_io_handle_t,
	esp_lcd_panel_io_event_data_t *,
	void *)
{
	BaseType_t taskWoken = pdFALSE;
	xSemaphoreGiveFromISR(transferComplete, &taskWoken);
	return taskWoken == pdTRUE;
}

void drawNativeBitmap(int x, int y, int width, int height, const uint16_t *pixels)
{
	if (!displayReady || width <= 0 || height <= 0)
	{
		return;
	}

	while (xSemaphoreTake(transferComplete, 0) == pdTRUE)
	{
	}

	ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(lcdPanel, x, y, x + width, y + height, pixels));
	xSemaphoreTake(transferComplete, portMAX_DELAY);
}

void fillRectangle(int x, int y, int width, int height, uint16_t color)
{
	if (!displayReady || frameBuffer == nullptr || width <= 0 || height <= 0)
	{
		return;
	}

	x = constrain(x, 0, SCREEN_WIDTH);
	y = constrain(y, 0, SCREEN_HEIGHT);
	width = min(width, SCREEN_WIDTH - x);
	height = min(height, SCREEN_HEIGHT - y);

	for (int row = 0; row < height; ++row)
	{
		uint16_t *destination = frameBuffer + static_cast<size_t>(y + row) * SCREEN_WIDTH + x;
		for (int column = 0; column < width; ++column)
		{
			destination[column] = color;
		}
	}
}

void presentDisplay(uint8_t verticalScalePercent = 100)
{
	if (!displayReady || frameBuffer == nullptr || transferBuffer == nullptr)
	{
		return;
	}

	verticalScalePercent = constrain(verticalScalePercent, 8, 100);
	const int scaledHeight = max(2, SCREEN_HEIGHT * verticalScalePercent / 100);
	const int scaledTop = (SCREEN_HEIGHT - scaledHeight) / 2;

	// The panel controller is natively 172x640, while the enclosure presents a
	// 640x172 landscape surface. Rotate the logical framebuffer 270 degrees into
	// native controller memory, matching Waveshare's LVGL implementation.
	for (int nativeY = 0; nativeY < LCD_NATIVE_HEIGHT; nativeY += LCD_CHUNK_HEIGHT)
	{
		const int chunkHeight = min(LCD_CHUNK_HEIGHT, LCD_NATIVE_HEIGHT - nativeY);
		for (int row = 0; row < chunkHeight; ++row)
		{
			const int physicalX = SCREEN_WIDTH - 1 - (nativeY + row);
			for (int nativeX = 0; nativeX < LCD_NATIVE_WIDTH; ++nativeX)
			{
				const int physicalY = nativeX;
				if (physicalY < scaledTop || physicalY >= scaledTop + scaledHeight)
				{
					transferBuffer[row * LCD_NATIVE_WIDTH + nativeX] = toPanelColor(COLOR_BACKGROUND);
					continue;
				}
				const int scaledPhysicalY = constrain(
					(physicalY - scaledTop) * SCREEN_HEIGHT / scaledHeight,
					0,
					SCREEN_HEIGHT - 1);
				const int logicalX = displayFlipped ? SCREEN_WIDTH - 1 - physicalX : physicalX;
				const int logicalY = displayFlipped ? SCREEN_HEIGHT - 1 - scaledPhysicalY : scaledPhysicalY;
				transferBuffer[row * LCD_NATIVE_WIDTH + nativeX] = toPanelColor(
					frameBuffer[static_cast<size_t>(logicalY) * SCREEN_WIDTH + logicalX]);
			}
		}
		drawNativeBitmap(0, nativeY, LCD_NATIVE_WIDTH, chunkHeight, transferBuffer);
	}
}

// ---------------------------------------------------------------------------
// LVGL presentation layer
// ---------------------------------------------------------------------------

constexpr uint32_t UI_BACKGROUND = 0x07111F;
constexpr uint32_t UI_SURFACE = 0x111E33;
constexpr uint32_t UI_SURFACE_RAISED = 0x192A47;
constexpr uint32_t UI_PRIMARY = 0x2F6FED;
constexpr uint32_t UI_PRIMARY_LIGHT = 0x72A7FF;
constexpr uint32_t UI_TEXT = 0xF7FAFF;
constexpr uint32_t UI_MUTED = 0x9DB0CB;
constexpr uint32_t UI_SUCCESS = 0x20C997;
constexpr uint32_t UI_WARNING = 0xF4B942;
constexpr uint32_t UI_DANGER = 0xFF5C68;

lv_color_t uiColor(uint32_t hex)
{
	return lv_color_hex(hex);
}

void lvglFlushDisplay(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
	const uint16_t *source = reinterpret_cast<const uint16_t *>(pixels);
	const int sourceWidth = lv_area_get_width(area);

	for (int nativeY = 0; nativeY < LCD_NATIVE_HEIGHT; nativeY += LCD_CHUNK_HEIGHT)
	{
		const int chunkHeight = min(LCD_CHUNK_HEIGHT, LCD_NATIVE_HEIGHT - nativeY);
		for (int row = 0; row < chunkHeight; ++row)
		{
			const int physicalX = SCREEN_WIDTH - 1 - (nativeY + row);
			for (int nativeX = 0; nativeX < LCD_NATIVE_WIDTH; ++nativeX)
			{
				const int physicalY = nativeX;
				const int logicalX = displayFlipped ? SCREEN_WIDTH - 1 - physicalX : physicalX;
				const int logicalY = displayFlipped ? SCREEN_HEIGHT - 1 - physicalY : physicalY;
				uint16_t color = 0;
				if (logicalX >= area->x1 && logicalX <= area->x2 &&
					logicalY >= area->y1 && logicalY <= area->y2)
				{
					const size_t sourceIndex = static_cast<size_t>(logicalY - area->y1) * sourceWidth +
						(logicalX - area->x1);
					color = source[sourceIndex];
				}
				transferBuffer[row * LCD_NATIVE_WIDTH + nativeX] = toPanelColor(color);
			}
		}
		drawNativeBitmap(0, nativeY, LCD_NATIVE_WIDTH, chunkHeight, transferBuffer);
	}
	lv_display_flush_ready(display);
}

void wakeFromClockAsync(void *)
{
	idleClockActive = false;
	lastClockSecond = -1;
	lastInteractionMs = millis();
	renderLvglPage(currentPage);
}

void lvglReadTouch(lv_indev_t *, lv_indev_data_t *data)
{
	uint16_t x = 0;
	uint16_t y = 0;
	uint8_t points = 0;
	if (readTouch(x, y, points))
	{
		data->state = LV_INDEV_STATE_PRESSED;
		data->point.x = x;
		data->point.y = y;
		lastInteractionMs = millis();
		if (idleClockActive)
			lv_async_call(wakeFromClockAsync, nullptr);
	}
	else
	{
		data->state = LV_INDEV_STATE_RELEASED;
	}
}

void styleScreen(lv_obj_t *screen, uint32_t background = UI_BACKGROUND)
{
	lv_obj_set_style_bg_color(screen, uiColor(background), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(screen, 0, 0);
	lv_obj_set_style_pad_all(screen, 0, 0);
	lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *makePanel(lv_obj_t *parent, int x, int y, int width, int height,
	uint32_t background = UI_SURFACE, int radius = 14)
{
	lv_obj_t *panel = lv_obj_create(parent);
	lv_obj_set_pos(panel, x, y);
	lv_obj_set_size(panel, width, height);
	lv_obj_set_style_bg_color(panel, uiColor(background), 0);
	lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(panel, uiColor(0x263B5C), 0);
	lv_obj_set_style_border_width(panel, 1, 0);
	lv_obj_set_style_radius(panel, radius, 0);
	lv_obj_set_style_pad_all(panel, 0, 0);
	lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
	return panel;
}

lv_obj_t *makeLabel(lv_obj_t *parent, const String &text, const lv_font_t *font,
	uint32_t color, int x, int y)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, text.c_str());
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, uiColor(color), 0);
	lv_obj_set_pos(label, x, y);
	return label;
}

void makeLabelFit(lv_obj_t *label, int width)
{
	lv_obj_set_width(label, width);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

void makeClickable(lv_obj_t *object, lv_event_cb_t callback, void *data = nullptr)
{
	lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, data);
	lv_obj_set_ext_click_area(object, 4);
}

uint32_t alertSeverityColor(const String &severity)
{
	if (severity == "critical")
		return UI_DANGER;
	if (severity == "warning")
		return UI_WARNING;
	return UI_PRIMARY_LIGHT;
}

void uiNavigate(lv_event_t *event)
{
	currentPage = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
	idleClockActive = false;
	lastInteractionMs = millis();
	renderLvglPage(currentPage);
}

void uiOpenLatestAlert(lv_event_t *)
{
	if (alertCount == 0)
		return;
	selectedAlertIndex = 0;
	currentPage = 1;
	renderLvglPage(currentPage);
}

void uiAlertNewer(lv_event_t *)
{
	if (selectedAlertIndex > 0)
		--selectedAlertIndex;
	renderLvglPage(1);
}

void uiAlertOlder(lv_event_t *)
{
	if (selectedAlertIndex + 1 < alertCount)
		++selectedAlertIndex;
	renderLvglPage(1);
}

void uiMarkAlertRead(lv_event_t *)
{
	NotificationPreview *alert = selectedAlert();
	if (alert != nullptr)
		alert->unread = false;
	renderLvglPage(1);
}

enum class DeviceSettingAction : uintptr_t
{
	BrightnessDown = 1,
	BrightnessUp,
	VolumeDown,
	VolumeUp,
};

/** Apply one bounded hardware setting and persist it immediately. */
void uiAdjustDeviceSetting(lv_event_t *event)
{
	const auto action = static_cast<DeviceSettingAction>(
		reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
	bool previewSound = false;

	switch (action)
	{
	case DeviceSettingAction::BrightnessDown:
		screenBrightnessPercent = max<uint8_t>(10, screenBrightnessPercent - SETTINGS_STEP);
		applyScreenBrightness();
		break;
	case DeviceSettingAction::BrightnessUp:
		screenBrightnessPercent = min<uint8_t>(100, screenBrightnessPercent + SETTINGS_STEP);
		applyScreenBrightness();
		break;
	case DeviceSettingAction::VolumeDown:
		soundVolumePercent = soundVolumePercent <= SETTINGS_STEP
			? 0
			: soundVolumePercent - SETTINGS_STEP;
		applySoundVolume();
		previewSound = soundVolumePercent > 0;
		break;
	case DeviceSettingAction::VolumeUp:
		soundVolumePercent = min<uint8_t>(100, soundVolumePercent + SETTINGS_STEP);
		applySoundVolume();
		previewSound = true;
		break;
	}

	saveDisplayConfiguration();
	lastInteractionMs = millis();
	renderLvglPage(3);
	if (previewSound)
		playTestChime();
}

void beginSetupAsync(void *)
{
	wifiWizardActive = false;
	wifiWizardStage = WifiWizardStage::Idle;
	wifiPasswordInput = nullptr;
	startSetupPortal();
}

void uiOpenSetup(lv_event_t *)
{
	// WiFiManager rebuilds network state and the active screen. Defer that work
	// until LVGL has finished dispatching the current button event.
	lv_async_call(beginSetupAsync, nullptr);
}

void renderWifiScanning();
void renderWifiResults();
void renderWifiPassword();
void renderWifiConnecting();
void renderWifiResult(bool connected);
void animateMqttDot(lv_obj_t *dot);

lv_obj_t *makeWizardButton(lv_obj_t *parent, const String &text, int x, int y,
	int width, lv_event_cb_t callback, uint32_t background = UI_PRIMARY, void *data = nullptr)
{
	lv_obj_t *button = makePanel(parent, x, y, width, 30, background, 11);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_t *label = makeLabel(button, text, &lv_font_montserrat_14, UI_TEXT, 0, 0);
	lv_obj_center(label);
	makeClickable(button, callback, data);
	return button;
}

void closeWifiWizardAsync(void *)
{
	wifiWizardActive = false;
	wifiWizardStage = WifiWizardStage::Idle;
	wifiPasswordInput = nullptr;
	lastInteractionMs = millis();
	renderLvglPage(3);
}

void uiCloseWifiWizard(lv_event_t *)
{
	lv_async_call(closeWifiWizardAsync, nullptr);
}

void scanWifiNetworksAsync(void *)
{
	const int found = WiFi.scanNetworks(false, true);
	wifiScanCount = 0;
	for (int index = 0; index < found && wifiScanCount < MAX_WIFI_SCAN_RESULTS; ++index)
	{
		const String ssid = WiFi.SSID(index);
		if (ssid.length() == 0)
			continue;
		bool duplicate = false;
		for (uint8_t saved = 0; saved < wifiScanCount; ++saved)
		{
			if (wifiScanSsids[saved] == ssid)
			{
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;
		wifiScanSsids[wifiScanCount] = ssid;
		wifiScanRssi[wifiScanCount] = WiFi.RSSI(index);
		wifiScanSecure[wifiScanCount] = WiFi.encryptionType(index) != WIFI_AUTH_OPEN;
		++wifiScanCount;
	}
	WiFi.scanDelete();
	wifiWizardStage = WifiWizardStage::Results;
	renderWifiResults();
}

void startWifiWizardAsync(void *)
{
	wifiWizardActive = true;
	wifiWizardStage = WifiWizardStage::Scanning;
	idleClockActive = false;
	lastInteractionMs = millis();
	renderWifiScanning();
	// Let LVGL paint the progress state before the synchronous radio scan.
	lv_async_call(scanWifiNetworksAsync, nullptr);
}

void uiStartWifiWizard(lv_event_t *)
{
	lv_async_call(startWifiWizardAsync, nullptr);
}

void uiRescanWifi(lv_event_t *)
{
	lv_async_call(startWifiWizardAsync, nullptr);
}

void selectWifiNetworkAsync(void *data)
{
	const uint8_t index = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(data));
	if (index >= wifiScanCount)
		return;
	pendingWifiSsid = wifiScanSsids[index];
	pendingWifiPassword = "";
	wifiPasswordDraft = "";
	wifiKeyboardMode = 0;
	pendingWifiOpen = !wifiScanSecure[index];
	wifiWizardStage = WifiWizardStage::Password;
	renderWifiPassword();
}

void uiSelectWifiNetwork(lv_event_t *event)
{
	lv_async_call(selectWifiNetworkAsync, lv_event_get_user_data(event));
}

void restorePreviousWifi()
{
	WiFi.disconnect(false, false);
	if (previousWifiSsid.length() > 0)
	{
		WiFi.begin(previousWifiSsid.c_str(), previousWifiPassword.c_str());
		Serial.printf("[WIFI] Restoring previous network: %s\n", previousWifiSsid.c_str());
	}
}

void beginWifiConnectionAsync(void *)
{
	if (pendingWifiSsid.length() == 0)
		return;
	if (!pendingWifiOpen)
		pendingWifiPassword = wifiPasswordDraft;

	// Snapshot the working station before WiFi.begin() replaces the persisted
	// credentials. On failure, restore this exact pair automatically.
	previousWifiSsid = WiFi.SSID();
	previousWifiPassword = WiFi.psk();
	wifiPasswordInput = nullptr;
	wifiWizardStage = WifiWizardStage::Connecting;
	renderWifiConnecting();
	mqttClient.disconnect();
	WiFi.disconnect(false, false);
	WiFi.mode(WIFI_STA);
	WiFi.begin(pendingWifiSsid.c_str(), pendingWifiOpen ? nullptr : pendingWifiPassword.c_str());
	wifiConnectStartedMs = millis();
	Serial.printf("[WIFI] Testing selected network: %s\n", pendingWifiSsid.c_str());
}

void uiConnectSelectedWifi(lv_event_t *)
{
	lv_async_call(beginWifiConnectionAsync, nullptr);
}

void retryWifiPasswordAsync(void *)
{
	wifiWizardStage = WifiWizardStage::Password;
	renderWifiPassword();
}

void uiRetryWifiPassword(lv_event_t *)
{
	lv_async_call(retryWifiPasswordAsync, nullptr);
}

void refreshWifiPasswordAsync(void *)
{
	renderWifiPassword();
}

void uiWifiKeyboardKey(lv_event_t *event)
{
	const char *key = static_cast<const char *>(lv_event_get_user_data(event));
	if (key == nullptr)
		return;
	if (strcmp(key, "BKSP") == 0)
	{
		if (wifiPasswordDraft.length() > 0)
			wifiPasswordDraft.remove(wifiPasswordDraft.length() - 1);
	}
	else if (strcmp(key, "SHIFT") == 0)
	{
		wifiKeyboardMode = wifiKeyboardMode == 1 ? 0 : 1;
		lv_async_call(refreshWifiPasswordAsync, nullptr);
		return;
	}
	else if (strcmp(key, "SYM") == 0 || strcmp(key, "ABC") == 0)
	{
		wifiKeyboardMode = strcmp(key, "SYM") == 0 ? 2 : 0;
		lv_async_call(refreshWifiPasswordAsync, nullptr);
		return;
	}
	else if (strcmp(key, "SPACE") == 0)
	{
		if (wifiPasswordDraft.length() < 63)
			wifiPasswordDraft += ' ';
	}
	else if (wifiPasswordDraft.length() < 63)
	{
		wifiPasswordDraft += key;
	}
	if (wifiPasswordInput != nullptr)
	{
		lv_textarea_set_text(wifiPasswordInput, wifiPasswordDraft.c_str());
		lv_textarea_set_cursor_pos(wifiPasswordInput, LV_TEXTAREA_CURSOR_LAST);
	}
}

void buildWifiKeyboard(lv_obj_t *parent)
{
	static const char *lower0[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
	static const char *lower1[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"};
	static const char *lower2[] = {"a", "s", "d", "f", "g", "h", "j", "k", "l", "BKSP"};
	static const char *lower3[] = {"SHIFT", "SYM", "z", "x", "c", "v", "b", "n", "m", "-", "_", "."};
	static const char *upper1[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"};
	static const char *upper2[] = {"A", "S", "D", "F", "G", "H", "J", "K", "L", "BKSP"};
	static const char *upper3[] = {"SHIFT", "SYM", "Z", "X", "C", "V", "B", "N", "M", "-", "_", "."};
	static const char *symbol0[] = {"!", "@", "#", "$", "%", "^", "&", "*", "(", ")"};
	static const char *symbol1[] = {"~", "`", "+", "=", "{", "}", "[", "]", "|", "\\"};
	static const char *symbol2[] = {":", ";", "\"", "'", "<", ">", "?", ",", "/", "BKSP"};
	static const char *symbol3[] = {"ABC", "SPACE", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};

	const char **rows[4] = {};
	uint8_t counts[] = {10, 10, 10, 12};
	if (wifiKeyboardMode == 2)
	{
		rows[0] = symbol0;
		rows[1] = symbol1;
		rows[2] = symbol2;
		rows[3] = symbol3;
	}
	else
	{
		rows[0] = lower0;
		rows[1] = wifiKeyboardMode == 1 ? upper1 : lower1;
		rows[2] = wifiKeyboardMode == 1 ? upper2 : lower2;
		rows[3] = wifiKeyboardMode == 1 ? upper3 : lower3;
	}

	constexpr int keyboardX = 2;
	constexpr int keyboardWidth = 636;
	constexpr int keyGap = 2;
	constexpr int keyHeight = 17;
	for (uint8_t row = 0; row < 4; ++row)
	{
		const int keyWidth = (keyboardWidth - keyGap * (counts[row] - 1)) / counts[row];
		for (uint8_t column = 0; column < counts[row]; ++column)
		{
			const char *key = rows[row][column];
			const bool action = strcmp(key, "BKSP") == 0 || strcmp(key, "SHIFT") == 0 ||
				strcmp(key, "SYM") == 0 || strcmp(key, "ABC") == 0 || strcmp(key, "SPACE") == 0;
			lv_obj_t *button = makePanel(parent,
				keyboardX + column * (keyWidth + keyGap), 94 + row * 19,
				keyWidth, keyHeight, action ? UI_PRIMARY : UI_SURFACE_RAISED, 4);
			lv_obj_set_style_border_width(button, 0, 0);
			String labelText = key;
			if (strcmp(key, "BKSP") == 0)
				labelText = "DEL";
			else if (strcmp(key, "SHIFT") == 0)
				labelText = wifiKeyboardMode == 1 ? "abc" : "ABC";
			lv_obj_t *label = makeLabel(button, labelText, &lv_font_montserrat_14,
				UI_TEXT, 0, 0);
			lv_obj_center(label);
			makeClickable(button, uiWifiKeyboardKey, const_cast<char *>(key));
		}
	}
}

void renderWifiShell(const String &title, const String &subtitle)
{
	lv_obj_t *screen = lv_screen_active();
	lv_obj_clean(screen);
	styleScreen(screen);
	makeLabel(screen, title, &lv_font_montserrat_20, UI_TEXT, 16, 9);
	makeLabel(screen, subtitle, &lv_font_montserrat_14, UI_MUTED, 16, 34);
}

void renderWifiScanning()
{
	renderWifiShell("Choose a Wi-Fi network", "Looking for nearby networks...");
	lv_obj_t *panel = makePanel(lv_screen_active(), 16, 64, 608, 82, UI_SURFACE, 16);
	lv_obj_t *dot = makePanel(panel, 20, 31, 16, 16, UI_PRIMARY, 8);
	lv_obj_set_style_border_width(dot, 0, 0);
	animateMqttDot(dot);
	makeLabel(panel, "Scanning", &lv_font_montserrat_20, UI_TEXT, 52, 17);
	makeLabel(panel, "This normally takes a few seconds.", &lv_font_montserrat_14, UI_MUTED, 52, 46);
	lv_obj_invalidate(lv_screen_active());
	lv_refr_now(lvglDisplay);
}

void renderWifiResults()
{
	renderWifiShell("Choose a Wi-Fi network", String(wifiScanCount) + " networks available");
	makeWizardButton(lv_screen_active(), "Back", 474, 8, 68, uiCloseWifiWizard, UI_SURFACE_RAISED);
	makeWizardButton(lv_screen_active(), "Rescan", 550, 8, 74, uiRescanWifi);

	if (wifiScanCount == 0)
	{
		makeLabel(lv_screen_active(), "No networks found. Move closer to the access point or rescan.",
			&lv_font_montserrat_16, UI_MUTED, 16, 82);
		return;
	}
	lv_obj_t *list = makePanel(lv_screen_active(), 10, 58, 620, 104, UI_SURFACE, 14);
	lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_style_pad_all(list, 6, 0);
	for (uint8_t index = 0; index < wifiScanCount; ++index)
	{
		lv_obj_t *row = makePanel(list, 6, index * 38 + 5, 594, 34,
			index == 0 ? 0x162B4B : UI_SURFACE_RAISED, 10);
		lv_obj_t *ssid = makeLabel(row, wifiScanSsids[index], &lv_font_montserrat_16,
			UI_TEXT, 12, 6);
		makeLabelFit(ssid, 390);
		String signal = String(wifiScanRssi[index]) + " dBm  " +
			(wifiScanSecure[index] ? "Secured" : "Open");
		makeLabel(row, signal, &lv_font_montserrat_14,
			wifiScanSecure[index] ? UI_MUTED : UI_WARNING, 418, 7);
		makeClickable(row, uiSelectWifiNetwork,
			reinterpret_cast<void *>(static_cast<uintptr_t>(index)));
	}
	lv_obj_invalidate(lv_screen_active());
	lv_refr_now(lvglDisplay);
}

void renderWifiPassword()
{
	renderWifiShell("Connect to " + pendingWifiSsid,
		pendingWifiOpen ? "This network does not require a password." : "Enter the network password");
	makeWizardButton(lv_screen_active(), "Back", 474, 8, 68, uiRescanWifi, UI_SURFACE_RAISED);
	makeWizardButton(lv_screen_active(), "Connect", 550, 8, 74, uiConnectSelectedWifi);
	if (pendingWifiOpen)
	{
		lv_obj_t *panel = makePanel(lv_screen_active(), 16, 68, 608, 78, UI_SURFACE, 16);
		makeLabel(panel, "Open network", &lv_font_montserrat_20, UI_WARNING, 18, 14);
		makeLabel(panel, "Traffic on open Wi-Fi may not be private. MQTT remains protected by TLS.",
			&lv_font_montserrat_14, UI_MUTED, 18, 45);
		return;
	}

	wifiPasswordInput = lv_textarea_create(lv_screen_active());
	lv_obj_set_pos(wifiPasswordInput, 0, 54);
	lv_obj_set_size(wifiPasswordInput, 640, 38);
	lv_textarea_set_one_line(wifiPasswordInput, true);
	// The compact physical display makes mistaps more likely. Keep the entered
	// value visible so users can verify it before replacing saved credentials.
	lv_textarea_set_password_mode(wifiPasswordInput, false);
	lv_textarea_set_max_length(wifiPasswordInput, 63);
	lv_textarea_set_placeholder_text(wifiPasswordInput, "Wi-Fi password");
	lv_textarea_set_text(wifiPasswordInput, wifiPasswordDraft.c_str());
	lv_obj_set_style_text_font(wifiPasswordInput, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(wifiPasswordInput, uiColor(UI_TEXT), 0);
	lv_obj_set_style_bg_color(wifiPasswordInput, uiColor(UI_SURFACE), 0);
	lv_obj_set_style_bg_opa(wifiPasswordInput, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(wifiPasswordInput, uiColor(UI_PRIMARY), 0);
	lv_obj_set_style_border_width(wifiPasswordInput, 2, 0);
	lv_obj_set_style_radius(wifiPasswordInput, 10, 0);
	lv_obj_set_style_pad_left(wifiPasswordInput, 12, 0);
	lv_obj_set_style_pad_top(wifiPasswordInput, 6, 0);

	buildWifiKeyboard(lv_screen_active());
	lv_obj_invalidate(lv_screen_active());
	lv_refr_now(lvglDisplay);
}

void renderWifiConnecting()
{
	renderWifiShell("Testing " + pendingWifiSsid, "Your saved network is kept until this succeeds.");
	lv_obj_t *panel = makePanel(lv_screen_active(), 16, 64, 608, 82, UI_SURFACE, 16);
	lv_obj_t *dot = makePanel(panel, 20, 31, 16, 16, UI_PRIMARY, 8);
	lv_obj_set_style_border_width(dot, 0, 0);
	animateMqttDot(dot);
	makeLabel(panel, "Connecting securely", &lv_font_montserrat_20, UI_TEXT, 52, 17);
	makeLabel(panel, "The previous network will be restored if this fails.",
		&lv_font_montserrat_14, UI_MUTED, 52, 46);
	lv_obj_invalidate(lv_screen_active());
	lv_refr_now(lvglDisplay);
}

void renderWifiResult(bool connected)
{
	renderWifiShell(connected ? "Wi-Fi connected" : "Could not connect",
		connected ? pendingWifiSsid : "The previous network is being restored.");
	lv_obj_t *panel = makePanel(lv_screen_active(), 16, 64, 608, 82,
		connected ? 0x113A37 : 0x3A202B, 16);
	makeLabel(panel, connected ? "Connection saved" : "Check the password and try again",
		&lv_font_montserrat_20, connected ? UI_SUCCESS : UI_DANGER, 18, 14);
	makeLabel(panel,
		connected ? "The device will reconnect to MQTT automatically."
			: "You can retry here or use the phone setup portal.",
		&lv_font_montserrat_14, UI_MUTED, 18, 45);
	if (connected)
		makeWizardButton(panel, "Done", 494, 26, 96, uiCloseWifiWizard);
	else
	{
		makeWizardButton(panel, "Retry", 390, 26, 90, uiRetryWifiPassword);
		makeWizardButton(panel, "Phone setup", 488, 26, 104, uiOpenSetup, UI_SURFACE_RAISED);
	}
	lv_obj_invalidate(lv_screen_active());
	lv_refr_now(lvglDisplay);
}

void animateMqttDot(lv_obj_t *dot)
{
	lv_anim_t animation;
	lv_anim_init(&animation);
	lv_anim_set_var(&animation, dot);
	lv_anim_set_values(&animation, LV_OPA_70, LV_OPA_COVER);
	lv_anim_set_duration(&animation, 850);
	lv_anim_set_playback_duration(&animation, 850);
	lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
	lv_anim_set_exec_cb(&animation, [](void *object, int32_t opacity)
		{
			lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), static_cast<lv_opa_t>(opacity), 0);
		});
	lv_anim_start(&animation);
}

void buildHeader(lv_obj_t *root, const String &section)
{
	makeLabel(root, "Notificator", &lv_font_montserrat_20, UI_TEXT, 14, 7);
	makeLabel(root, section, &lv_font_montserrat_14, UI_PRIMARY_LIGHT, 143, 11);

	const bool wifiReady = WiFi.isConnected();
	const bool mqttHealthy = mqttClient.connected() && millis() - lastMqttHealthMs <= MQTT_HEALTH_GRACE_MS;
	lv_obj_t *wifiPill = makePanel(root, 338, 6, 84, 28, UI_SURFACE, 12);
	lv_obj_set_style_border_width(wifiPill, 0, 0);
	lv_obj_t *wifiDot = makePanel(wifiPill, 10, 10, 8, 8, wifiReady ? UI_SUCCESS : UI_DANGER, 4);
	lv_obj_set_style_border_width(wifiDot, 0, 0);
	makeLabel(wifiPill, wifiReady ? "Wi-Fi" : "Offline", &lv_font_montserrat_14,
		wifiReady ? UI_MUTED : UI_DANGER, 25, 5);

	lv_obj_t *mqttPill = makePanel(root, 428, 6, 84, 28, UI_SURFACE, 12);
	lv_obj_set_style_border_width(mqttPill, 0, 0);
	lv_obj_t *mqttDot = makePanel(mqttPill, 10, 10, 8, 8, mqttHealthy ? UI_SUCCESS : UI_DANGER, 4);
	lv_obj_set_style_border_width(mqttDot, 0, 0);
	makeLabel(mqttPill, mqttHealthy ? "MQTT" : "Lost", &lv_font_montserrat_14,
		mqttHealthy ? UI_MUTED : UI_DANGER, 25, 5);
	if (mqttHealthy)
		animateMqttDot(mqttDot);

	String power = batteryAvailable ? String(batteryPercent) + "%" : "USB";
	lv_obj_t *batteryPill = makePanel(root, 518, 6, 112, 28, UI_SURFACE, 12);
	lv_obj_set_style_border_width(batteryPill, 0, 0);
	makeLabel(batteryPill, "BAT", &lv_font_montserrat_14, UI_MUTED, 10, 5);
	makeLabel(batteryPill, power, &lv_font_montserrat_14,
		batteryAvailable && batteryPercent <= 15 ? UI_DANGER : UI_TEXT, 52, 5);
}

void buildNavigation(lv_obj_t *root, uint8_t active)
{
	lv_obj_t *bar = makePanel(root, 8, 138, 624, 30, UI_SURFACE, 12);
	lv_obj_set_style_border_width(bar, 0, 0);
	const char *labels[] = {"Home", "Alerts", "Device", "Settings"};
	for (uint8_t index = 0; index < 4; ++index)
	{
		lv_obj_t *button = makePanel(bar, index * 153 + 3, 3, 149, 24,
			index == active ? UI_PRIMARY : UI_SURFACE, 10);
		lv_obj_set_style_border_width(button, 0, 0);
		lv_obj_t *label = makeLabel(button, labels[index], &lv_font_montserrat_14,
			index == active ? UI_TEXT : UI_MUTED, 0, 1);
		lv_obj_center(label);
		makeClickable(button, uiNavigate, reinterpret_cast<void *>(static_cast<uintptr_t>(index)));
		if (index == 1 && unreadAlertCount() > 0)
		{
			lv_obj_t *badge = makePanel(button, 112, 3, 20, 18, UI_DANGER, 9);
			lv_obj_set_style_border_width(badge, 0, 0);
			lv_obj_t *count = makeLabel(badge, String(unreadAlertCount()), &lv_font_montserrat_14,
				UI_TEXT, 0, 0);
			lv_obj_center(count);
		}
	}
}

void buildHome(lv_obj_t *root)
{
	NotificationPreview *latest = alertCount > 0 ? &alertHistory[0] : nullptr;
	// A solid color avoids visible RGB565 gradient bands on this narrow panel.
	lv_obj_t *alertCard = makePanel(root, 10, 40, 416, 94, 0x122D56, 16);
	if (latest != nullptr)
	{
		const uint32_t severityColor = alertSeverityColor(latest->severity);
		lv_obj_t *accent = makePanel(alertCard, 0, 0, 6, 94, severityColor, 3);
		lv_obj_set_style_border_width(accent, 0, 0);
		String status = latest->severity;
		status.toUpperCase();
		status += latest->unread ? "  •  UNREAD" : "  •  READ";
		makeLabel(alertCard, status, &lv_font_montserrat_14, severityColor, 16, 8);
		makeLabel(alertCard, latest->receivedAt, &lv_font_montserrat_14, UI_MUTED, 344, 8);
		lv_obj_t *title = makeLabel(alertCard, latest->title, &lv_font_montserrat_20, UI_TEXT, 16, 31);
		makeLabelFit(title, 382);
		lv_obj_t *body = makeLabel(alertCard, latest->body, &lv_font_montserrat_14, UI_MUTED, 16, 60);
		makeLabelFit(body, 382);
		makeClickable(alertCard, uiOpenLatestAlert);
	}
	else
	{
		makeLabel(alertCard, "SYSTEM READY", &lv_font_montserrat_14, UI_SUCCESS, 16, 9);
		makeLabel(alertCard, "All caught up", &lv_font_montserrat_24, UI_TEXT, 16, 32);
		makeLabel(alertCard, "Your next site event will appear here.", &lv_font_montserrat_14,
			UI_MUTED, 16, 65);
	}

	lv_obj_t *unreadCard = makePanel(root, 436, 40, 194, 44, UI_SURFACE, 13);
	makeLabel(unreadCard, "UNREAD", &lv_font_montserrat_14, UI_MUTED, 14, 13);
	makeLabel(unreadCard, String(unreadAlertCount()), &lv_font_montserrat_28,
		unreadAlertCount() > 0 ? UI_PRIMARY_LIGHT : UI_TEXT, 146, 5);
	makeClickable(unreadCard, uiOpenLatestAlert);

	const bool mqttHealthy = mqttClient.connected() && millis() - lastMqttHealthMs <= MQTT_HEALTH_GRACE_MS;
	lv_obj_t *mqttCard = makePanel(root, 436, 90, 194, 44,
		mqttHealthy ? UI_SURFACE : UI_PRIMARY, 13);
	lv_obj_t *dot = makePanel(mqttCard, 14, 17, 10, 10,
		mqttHealthy ? UI_SUCCESS : UI_DANGER, 5);
	lv_obj_set_style_border_width(dot, 0, 0);
	makeLabel(mqttCard, mqttHealthy ? "MQTT live" : "Connect MQTT",
		&lv_font_montserrat_16, UI_TEXT, 34, 11);
	if (mqttHealthy)
		animateMqttDot(dot);
	else
		makeClickable(mqttCard, uiOpenSetup);
}

void buildAlerts(lv_obj_t *root)
{
	NotificationPreview *alert = selectedAlert();
	lv_obj_t *card = makePanel(root, 10, 40, 620, 94, UI_SURFACE_RAISED, 16);
	if (alert == nullptr)
	{
		makeLabel(card, "NO ALERTS", &lv_font_montserrat_14, UI_SUCCESS, 18, 10);
		makeLabel(card, "Nothing needs your attention", &lv_font_montserrat_20, UI_TEXT, 18, 34);
		makeLabel(card, "New events will be kept here while the device is powered.",
			&lv_font_montserrat_14, UI_MUTED, 18, 63);
		return;
	}

	const uint32_t severityColor = alertSeverityColor(alert->severity);
	lv_obj_t *accent = makePanel(card, 0, 0, 6, 94, severityColor, 3);
	lv_obj_set_style_border_width(accent, 0, 0);
	String status = alert->severity;
	status.toUpperCase();
	status += alert->unread ? "  •  UNREAD" : "  •  READ";
	makeLabel(card, status, &lv_font_montserrat_14, severityColor, 18, 7);
	makeLabel(card, String(selectedAlertIndex + 1) + " / " + String(alertCount),
		&lv_font_montserrat_14, UI_MUTED, 350, 7);
	lv_obj_t *title = makeLabel(card, alert->title, &lv_font_montserrat_20, UI_TEXT, 18, 28);
	makeLabelFit(title, 390);
	lv_obj_t *body = makeLabel(card, alert->body, &lv_font_montserrat_14, UI_MUTED, 18, 58);
	makeLabelFit(body, 390);
	makeClickable(card, uiMarkAlertRead);

	lv_obj_t *newer = makePanel(card, 430, 12, 78, 30,
		selectedAlertIndex > 0 ? UI_PRIMARY : UI_SURFACE, 10);
	lv_obj_t *newerLabel = makeLabel(newer, "Newer", &lv_font_montserrat_14,
		selectedAlertIndex > 0 ? UI_TEXT : UI_MUTED, 0, 0);
	lv_obj_center(newerLabel);
	makeClickable(newer, uiAlertNewer);
	lv_obj_t *older = makePanel(card, 516, 12, 86, 30,
		selectedAlertIndex + 1 < alertCount ? UI_PRIMARY : UI_SURFACE, 10);
	lv_obj_t *olderLabel = makeLabel(older, "Older", &lv_font_montserrat_14,
		selectedAlertIndex + 1 < alertCount ? UI_TEXT : UI_MUTED, 0, 0);
	lv_obj_center(olderLabel);
	makeClickable(older, uiAlertOlder);
	makeLabel(card, alert->source, &lv_font_montserrat_14, UI_PRIMARY_LIGHT, 430, 58);
	makeLabel(card, alert->receivedAt, &lv_font_montserrat_14, UI_MUTED, 550, 58);
}

void buildDevice(lv_obj_t *root)
{
	const String batteryText = batteryAvailable
		? String(batteryPercent) + "%  " + String(batteryVoltage, 2) + "V"
		: "USB power";
	const String values[] = {displayPairingId(), FIRMWARE_LABEL, batteryText,
		mqttClient.connected() ? "Connected" : "Disconnected"};
	const String labels[] = {"DEVICE ID", "FIRMWARE", "BATTERY", "MQTT"};
	for (uint8_t index = 0; index < 4; ++index)
	{
		lv_obj_t *card = makePanel(root, 10 + index * 155, 40, 145, 94, UI_SURFACE, 14);
		makeLabel(card, labels[index], &lv_font_montserrat_14, UI_MUTED, 12, 12);
		lv_obj_t *value = makeLabel(card, values[index], &lv_font_montserrat_18,
			index == 3 ? (mqttClient.connected() ? UI_SUCCESS : UI_DANGER) : UI_TEXT, 12, 42);
		makeLabelFit(value, 121);
	}
}

void buildSettings(lv_obj_t *root)
{
	const bool wifiReady = WiFi.isConnected();
	String networkName = wifiReady ? WiFi.SSID() : "Not connected";
	if (networkName.length() == 0)
		networkName = "Saved network";

	lv_obj_t *networkCard = makePanel(root, 10, 40, 235, 94, UI_SURFACE, 15);
	makeLabel(networkCard, "WI-FI NETWORK", &lv_font_montserrat_14, UI_MUTED, 16, 10);
	lv_obj_t *network = makeLabel(networkCard, networkName, &lv_font_montserrat_20,
		wifiReady ? UI_TEXT : UI_DANGER, 16, 35);
	makeLabelFit(network, 203);
	lv_obj_t *dot = makePanel(networkCard, 16, 68, 9, 9,
		wifiReady ? UI_SUCCESS : UI_DANGER, 5);
	lv_obj_set_style_border_width(dot, 0, 0);
	makeLabel(networkCard, wifiReady ? "Connected" : "Offline", &lv_font_montserrat_14,
		wifiReady ? UI_SUCCESS : UI_DANGER, 33, 65);

	lv_obj_t *actionCard = makePanel(root, 255, 40, 190, 94, 0x122D56, 15);
	makeLabel(actionCard, "Change Wi-Fi network", &lv_font_montserrat_20, UI_TEXT, 16, 10);
	makeLabel(actionCard, "Connect on this screen.",
		&lv_font_montserrat_14, UI_MUTED, 16, 38);
	lv_obj_t *button = makePanel(actionCard, 16, 59, 158, 28, UI_PRIMARY, 11);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_t *buttonLabel = makeLabel(button, "Choose network", &lv_font_montserrat_14, UI_TEXT, 0, 0);
	lv_obj_center(buttonLabel);
	makeClickable(button, uiStartWifiWizard);

	lv_obj_t *controlsCard = makePanel(root, 455, 40, 175, 94, UI_SURFACE, 15);
	const char *labels[] = {"DISPLAY", "VOLUME"};
	const uint8_t values[] = {screenBrightnessPercent, soundVolumePercent};
	const DeviceSettingAction downActions[] = {
		DeviceSettingAction::BrightnessDown, DeviceSettingAction::VolumeDown};
	const DeviceSettingAction upActions[] = {
		DeviceSettingAction::BrightnessUp, DeviceSettingAction::VolumeUp};
	for (uint8_t index = 0; index < 2; ++index)
	{
		const int y = 8 + index * 42;
		makeLabel(controlsCard, labels[index], &lv_font_montserrat_14, UI_MUTED, 10, y + 5);
		makeLabel(controlsCard, String(values[index]) + "%", &lv_font_montserrat_14,
			UI_TEXT, 69, y + 5);
		lv_obj_t *minus = makePanel(controlsCard, 111, y, 24, 26, UI_SURFACE_RAISED, 8);
		lv_obj_t *minusLabel = makeLabel(minus, "-", &lv_font_montserrat_20, UI_TEXT, 0, 0);
		lv_obj_center(minusLabel);
		makeClickable(minus, uiAdjustDeviceSetting,
			reinterpret_cast<void *>(static_cast<uintptr_t>(downActions[index])));
		lv_obj_t *plus = makePanel(controlsCard, 141, y, 24, 26, UI_PRIMARY, 8);
		lv_obj_t *plusLabel = makeLabel(plus, "+", &lv_font_montserrat_20, UI_TEXT, 0, 0);
		lv_obj_center(plusLabel);
		makeClickable(plus, uiAdjustDeviceSetting,
			reinterpret_cast<void *>(static_cast<uintptr_t>(upActions[index])));
	}
}

void renderLvglPage(uint8_t page)
{
	if (!lvglReady)
		return;
	lv_obj_t *screen = lv_screen_active();
	lv_obj_clean(screen);
	styleScreen(screen);
	lvglPageRoot = screen;
	const String section = page == 0 ? "Home" : page == 1 ? "Alerts" : page == 2 ? "Device" : "Settings";
	buildHeader(screen, section);
	if (page == 0)
		buildHome(screen);
	else if (page == 1)
		buildAlerts(screen);
	else if (page == 2)
		buildDevice(screen);
	else
		buildSettings(screen);
	buildNavigation(screen, page);
	lv_obj_invalidate(screen);
	lv_refr_now(lvglDisplay);
}

void renderLvglClock()
{
	if (!lvglReady)
		return;
	// Keep LVGL responsible for the full-screen wake target, then draw the clock
	// directly into the RGB565 framebuffer. Native integer-scaled bitmap glyphs
	// remain razor sharp, unlike a transformed vector font on this 172 px panel.
	if (lastClockSecond < 0)
	{
		lv_obj_t *screen = lv_screen_active();
		lv_obj_clean(screen);
		styleScreen(screen, 0x000000);
		lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(screen, [](lv_event_t *) { lv_async_call(wakeFromClockAsync, nullptr); },
			LV_EVENT_CLICKED, nullptr);
		lv_obj_invalidate(screen);
		lv_refr_now(lvglDisplay);
	}

	fillRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
	tm clockTime = {};
	String timeText = "--:--";
	String dateText = WiFi.isConnected() ? "Syncing time" : "Connect Wi-Fi for time";
	if (readLocalClock(clockTime))
	{
		char timeBuffer[6] = {};
		char dateBuffer[28] = {};
		strftime(timeBuffer, sizeof(timeBuffer), "%H:%M", &clockTime);
		strftime(dateBuffer, sizeof(dateBuffer), "%A, %d %B", &clockTime);
		timeText = timeBuffer;
		dateText = dateBuffer;
		lastClockMinute = clockTime.tm_min;
		lastClockSecond = clockTime.tm_sec;
	}
	else
	{
		lastClockMinute = -1;
		lastClockSecond = static_cast<int>((millis() / 1000) % 60);
	}
	// Alternate the separator without scaling or animating the digit glyphs.
	// This mirrors a traditional digital clock and preserves the crisp bitmap.
	if ((lastClockSecond & 1) != 0 && timeText.length() >= 3)
		timeText.setCharAt(2, ' ');
	dateText.toUpperCase();
	String cityText = weatherCity.length() ? weatherCity : "WEATHER";
	cityText.toUpperCase();
	cityText = cityText.substring(0, 18);
	const String temperatureText = weatherHasData
		? String(static_cast<int>(roundf(weatherTemperatureC))) + "C"
		: "--C";
	const String conditionText = weatherHasData
		? weatherConditionLabel(weatherCode)
		: (WiFi.isConnected() ? "UPDATING" : "OFFLINE");

	if (idleTheme == 1)
	{
		drawCenteredText(190, 35, timeText, 9, COLOR_WHITE, COLOR_BLACK);
		drawCenteredText(190, 125, dateText, 2, COLOR_MUTED, COLOR_BLACK);
		fillRectangle(386, 22, 2, 128, COLOR_PANEL_RAISED);
		drawCenteredText(510, 23, cityText, 2, COLOR_BLUE_LIGHT, COLOR_BLACK);
		drawCenteredText(510, 55, temperatureText, 8,
			weatherHasData ? COLOR_WHITE : COLOR_MUTED, COLOR_BLACK);
		drawCenteredText(510, 132, conditionText, 2, COLOR_MUTED, COLOR_BLACK);
	}
	else if (idleTheme == 2)
	{
		drawCenteredText(SCREEN_WIDTH / 2, 18, cityText, 3, COLOR_BLUE_LIGHT, COLOR_BLACK);
		drawCenteredText(SCREEN_WIDTH / 2, 53, temperatureText, 9,
			weatherHasData ? COLOR_WHITE : COLOR_MUTED, COLOR_BLACK);
		drawCenteredText(SCREEN_WIDTH / 2, 139, conditionText, 2, COLOR_MUTED, COLOR_BLACK);
	}
	else
	{
		drawCenteredText(SCREEN_WIDTH / 2, 24, timeText, 13, COLOR_WHITE, COLOR_BLACK);
		drawCenteredText(SCREEN_WIDTH / 2, 140, dateText, 2, COLOR_MUTED, COLOR_BLACK);
	}
	presentDisplay();
}

void renderLvglSetup(const String &state)
{
	if (!lvglReady)
		return;
	lv_obj_t *screen = lv_screen_active();
	lv_obj_clean(screen);
	styleScreen(screen);
	lv_obj_t *identity = makePanel(screen, 10, 10, 210, 152, UI_PRIMARY, 18);
	lv_obj_set_style_border_width(identity, 0, 0);
	makeLabel(identity, "Notificator", &lv_font_montserrat_20, UI_TEXT, 18, 18);
	makeLabel(identity, "PAIRING ID", &lv_font_montserrat_14, 0xDCE8FF, 18, 62);
	makeLabel(identity, displayPairingId(), &lv_font_montserrat_24, UI_TEXT, 18, 84);
	makeLabel(identity, "Use this ID in the app", &lv_font_montserrat_14, 0xDCE8FF, 18, 120);

	makeLabel(screen, "Connect your display", &lv_font_montserrat_24, UI_TEXT, 246, 16);
	makeLabel(screen, "1", &lv_font_montserrat_20, UI_PRIMARY_LIGHT, 246, 55);
	makeLabel(screen, "Join " + setupSsid, &lv_font_montserrat_16, UI_TEXT, 276, 57);
	makeLabel(screen, "2", &lv_font_montserrat_20, UI_PRIMARY_LIGHT, 246, 88);
	makeLabel(screen, "Open 192.168.4.1 and add Wi-Fi + HiveMQ",
		&lv_font_montserrat_14, UI_MUTED, 276, 92);
	lvglSetupStatus = makeLabel(screen, state, &lv_font_montserrat_14, UI_SUCCESS, 246, 132);
	lv_obj_invalidate(screen);
	lv_refr_now(lvglDisplay);
}

void renderLvglSplash()
{
	lv_obj_t *screen = lv_screen_active();
	lv_obj_clean(screen);
	styleScreen(screen, UI_PRIMARY);
	lv_obj_t *title = makeLabel(screen, "Notificator", &lv_font_montserrat_32, UI_TEXT, 0, 45);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_y(title, 45);
	lv_obj_t *subtitle = makeLabel(screen, "Touch", &lv_font_montserrat_20, 0xDCE8FF, 0, 88);
	lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 88);
	makeLabel(screen, "Starting securely…", &lv_font_montserrat_14, 0xDCE8FF, 500, 144);
	lv_obj_invalidate(screen);
	lv_refr_now(lvglDisplay);
}

void initializeLvglUi()
{
	if (lvglReady || frameBuffer == nullptr)
		return;
	lv_init();
	lv_tick_set_cb([]() -> uint32_t { return millis(); });
	lvglDisplay = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
	lv_display_set_color_format(lvglDisplay, LV_COLOR_FORMAT_RGB565);
	lv_display_set_flush_cb(lvglDisplay, lvglFlushDisplay);
	lv_display_set_buffers(lvglDisplay, frameBuffer, nullptr,
		static_cast<uint32_t>(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t)),
		LV_DISPLAY_RENDER_MODE_FULL);
	lvglTouchInput = lv_indev_create();
	lv_indev_set_type(lvglTouchInput, LV_INDEV_TYPE_POINTER);
	lv_indev_set_display(lvglTouchInput, lvglDisplay);
	lv_indev_set_read_cb(lvglTouchInput, lvglReadTouch);
	lvglReady = true;
	renderLvglSplash();
	Serial.println("[UI] LVGL 9.3 initialized");
}

void animateOrientationChange(bool desiredFlip)
{
	if (lvglReady)
	{
		displayFlipped = desiredFlip;
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(lvglDisplay);
		return;
	}
	constexpr uint8_t collapseFrames[] = {100, 54, 10};
	constexpr uint8_t expandFrames[] = {10, 54, 100};
	for (uint8_t scale : collapseFrames)
	{
		presentDisplay(scale);
		delay(10);
	}

	displayFlipped = desiredFlip;
	deferPagePresentation = true;
	drawCurrentPage();
	deferPagePresentation = false;
	for (uint8_t scale : expandFrames)
	{
		presentDisplay(scale);
		delay(10);
	}
}

void drawOutline(int x, int y, int width, int height, uint16_t color, int thickness = 2)
{
	fillRectangle(x, y, width, thickness, color);
	fillRectangle(x, y + height - thickness, width, thickness, color);
	fillRectangle(x, y, thickness, height, color);
	fillRectangle(x + width - thickness, y, thickness, height, color);
}

void fillCircle(int centerX, int centerY, int radius, uint16_t color)
{
	for (int y = -radius; y <= radius; ++y)
	{
		const int halfWidth = static_cast<int>(sqrtf(static_cast<float>(radius * radius - y * y)));
		fillRectangle(centerX - halfWidth, centerY + y, halfWidth * 2 + 1, 1, color);
	}
}

void fillRoundedRectangle(int x, int y, int width, int height, int radius, uint16_t color)
{
	radius = min(radius, min(width, height) / 2);
	fillRectangle(x + radius, y, width - radius * 2, height, color);
	fillRectangle(x, y + radius, width, height - radius * 2, color);
	for (int offset = 0; offset < radius; ++offset)
	{
		const int inset = radius - static_cast<int>(sqrtf(static_cast<float>(radius * radius - offset * offset)));
		fillRectangle(x + inset, y + radius - offset, width - inset * 2, 1, color);
		fillRectangle(x + inset, y + height - radius + offset - 1, width - inset * 2, 1, color);
	}
}

const uint8_t *glyphFor(char character)
{
	if (character >= '0' && character <= '9')
	{
		return DIGIT_GLYPHS[character - '0'];
	}
	if (character >= 'A' && character <= 'Z')
	{
		return LETTER_GLYPHS[character - 'A'];
	}
	return nullptr;
}

void drawCharacter(int x, int y, char character, int scale, uint16_t foreground, uint16_t background)
{
	constexpr int GLYPH_WIDTH = 6;
	constexpr int GLYPH_HEIGHT = 8;
	const int bitmapWidth = GLYPH_WIDTH * scale;
	const int bitmapHeight = GLYPH_HEIGHT * scale;
	const uint8_t *glyph = glyphFor(character);

	// Draw directly into PSRAM. The previous fixed local bitmap only held scales
	// up to 3 and corrupted the stack when the idle clock requested scale 10.
	for (int row = 0; row < bitmapHeight; ++row)
	{
		for (int column = 0; column < bitmapWidth; ++column)
		{
			const int sourceColumn = column / scale;
			const int sourceRow = row / scale;
			bool enabled = false;

			if (glyph != nullptr && sourceColumn < 5 && sourceRow < 7)
			{
				enabled = (glyph[sourceColumn] & (1U << sourceRow)) != 0;
			}
			else if (character == '-' && sourceRow == 3 && sourceColumn < 5)
			{
				enabled = true;
			}
			else if (character == '.' && sourceColumn == 2 && sourceRow == 6)
			{
				enabled = true;
			}
			else if (character == ':' && sourceColumn == 2 && (sourceRow == 2 || sourceRow == 5))
			{
				enabled = true;
			}
			else if (character == '%' &&
				((sourceColumn == 0 && sourceRow == 1) || (sourceColumn == 4 && sourceRow == 5) ||
					(sourceColumn == 4 - sourceRow * 2 / 3 && sourceRow >= 1 && sourceRow <= 5)))
			{
				enabled = true;
			}
			const int targetX = x + column;
			const int targetY = y + row;
			if (targetX >= 0 && targetX < SCREEN_WIDTH && targetY >= 0 && targetY < SCREEN_HEIGHT)
			{
				frameBuffer[static_cast<size_t>(targetY) * SCREEN_WIDTH + targetX] =
					enabled ? foreground : background;
			}
		}
	}
}

void drawText(int x, int y, const String &text, int scale, uint16_t foreground, uint16_t background)
{
	for (size_t index = 0; index < text.length(); ++index)
	{
		drawCharacter(x + static_cast<int>(index) * 6 * scale, y, text[index], scale, foreground, background);
	}
}

int textWidth(const String &text, int scale)
{
	return static_cast<int>(text.length()) * 6 * scale;
}

String pairingId()
{
	String value = deviceId;
	value.toLowerCase();
	return value;
}

/** Return an uppercase ID supported by the display's compact bitmap font. */
String displayPairingId()
{
	String value = pairingId();
	value.toUpperCase();
	return value;
}

void drawCenteredText(int centerX, int y, const String &text, int scale, uint16_t foreground, uint16_t background)
{
	drawText(centerX - textWidth(text, scale) / 2, y, text, scale, foreground, background);
}

void safeCopy(char *target, size_t size, const String &value)
{
	if (target == nullptr || size == 0)
	{
		return;
	}
	strlcpy(target, value.c_str(), size);
}

/** Convert an open-circuit Li-ion voltage estimate into a useful percentage. */
uint8_t estimateBatteryPercent(float voltage)
{
	struct BatteryPoint
	{
		float voltage;
		uint8_t percent;
	};
	static constexpr BatteryPoint CURVE[] = {
		{3.30F, 0}, {3.45F, 5}, {3.60F, 12}, {3.70F, 25}, {3.75F, 40},
		{3.80F, 55}, {3.90F, 70}, {4.00F, 82}, {4.10F, 92}, {4.20F, 100},
	};

	if (voltage <= CURVE[0].voltage)
		return 0;
	for (size_t index = 1; index < sizeof(CURVE) / sizeof(CURVE[0]); ++index)
	{
		if (voltage <= CURVE[index].voltage)
		{
			const BatteryPoint &low = CURVE[index - 1];
			const BatteryPoint &high = CURVE[index];
			const float position = (voltage - low.voltage) / (high.voltage - low.voltage);
			return static_cast<uint8_t>(roundf(low.percent + position * (high.percent - low.percent)));
		}
	}
	return 100;
}

/** Sample the board's documented GPIO4 battery divider and smooth the result. */
bool sampleBatteryLevel()
{
	constexpr uint8_t SAMPLE_COUNT = 12;
	uint32_t millivoltTotal = 0;
	for (uint8_t sample = 0; sample < SAMPLE_COUNT; ++sample)
	{
		millivoltTotal += analogReadMilliVolts(BATTERY_ADC_PIN);
		delay(2);
	}

	const float measuredVoltage =
		(static_cast<float>(millivoltTotal) / SAMPLE_COUNT / 1000.0F) * BATTERY_DIVIDER_RATIO;
	const bool measuredBatteryAvailable = measuredVoltage >= 2.80F && measuredVoltage <= 4.50F;
	const bool availabilityChanged = measuredBatteryAvailable != batteryAvailable;
	batteryAvailable = measuredBatteryAvailable;
	lastBatterySampleMs = millis();
	if (!batteryAvailable)
	{
		batteryVoltage = 0.0F;
		batteryPercent = 0;
		return availabilityChanged;
	}

	const float previousVoltage = batteryVoltage;
	const uint8_t previousPercent = batteryPercent;
	batteryVoltage = previousVoltage > 0.0F
		? previousVoltage * 0.72F + measuredVoltage * 0.28F
		: measuredVoltage;
	batteryPercent = estimateBatteryPercent(batteryVoltage);
	return availabilityChanged || abs(static_cast<int>(batteryPercent) - previousPercent) >= 2;
}

void initializeBatteryMonitor()
{
	analogReadResolution(12);
	analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
	// The first calibrated ADC conversion initializes the Arduino ADC handle.
	// Sample once during boot so the first rendered header is already truthful.
	sampleBatteryLevel();
}

void startClockSync()
{
	if (!WiFi.isConnected() || clockSyncStarted)
		return;
	if (weatherTimezone.length())
		configTzTime(weatherTimezone.c_str(), "pool.ntp.org", "time.nist.gov");
	else
		configTime(static_cast<long>(clockUtcOffsetMinutes) * 60L, 0, "pool.ntp.org", "time.nist.gov");
	clockSyncStarted = true;
	Serial.printf("[CLOCK] NTP requested with %s\n",
		weatherTimezone.length() ? weatherTimezone.c_str() : "the saved UTC offset");
}

String urlEncode(const String &value)
{
	static const char hex[] = "0123456789ABCDEF";
	String encoded;
	encoded.reserve(value.length() * 3);
	for (size_t index = 0; index < value.length(); ++index)
	{
		const uint8_t character = static_cast<uint8_t>(value[index]);
		if (isalnum(character) || character == '-' || character == '_' || character == '.')
			encoded += static_cast<char>(character);
		else
		{
			encoded += '%';
			encoded += hex[(character >> 4) & 0x0f];
			encoded += hex[character & 0x0f];
		}
	}
	return encoded;
}

String weatherConditionLabel(uint8_t code)
{
	if (code == 0)
		return "CLEAR";
	if (code <= 3)
		return "CLOUDY";
	if (code == 45 || code == 48)
		return "FOG";
	if (code >= 51 && code <= 67)
		return "RAIN";
	if (code >= 71 && code <= 77)
		return "SNOW";
	if (code >= 80 && code <= 82)
		return "SHOWERS";
	if (code >= 95)
		return "STORM";
	return "WEATHER";
}

bool resolveWeatherCity()
{
	if (weatherHasCoordinates || !weatherCity.length() || !WiFi.isConnected())
		return weatherHasCoordinates;
	WiFiClientSecure client;
	client.setInsecure();
	HTTPClient http;
	const String url = "https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&name=" +
		urlEncode(weatherCity);
	if (!http.begin(client, url))
		return false;
	http.setConnectTimeout(8000);
	http.setTimeout(10000);
	const int status = http.GET();
	if (status != HTTP_CODE_OK)
	{
		http.end();
		return false;
	}
	JsonDocument document;
	const DeserializationError error = deserializeJson(document, http.getString());
	http.end();
	JsonObject result = document["results"][0].as<JsonObject>();
	if (error || result.isNull())
		return false;
	weatherLatitude = result["latitude"] | 0.0F;
	weatherLongitude = result["longitude"] | 0.0F;
	weatherHasCoordinates = !(weatherLatitude == 0.0F && weatherLongitude == 0.0F);
	return weatherHasCoordinates;
}

bool fetchWeatherNow()
{
	if (!WiFi.isConnected() || !resolveWeatherCity())
		return false;
	WiFiClientSecure client;
	client.setInsecure();
	HTTPClient http;
	const String url = "https://api.open-meteo.com/v1/forecast?latitude=" +
		String(weatherLatitude, 4) + "&longitude=" + String(weatherLongitude, 4) +
		"&current=temperature_2m,wind_speed_10m,weather_code&timezone=auto";
	if (!http.begin(client, url))
		return false;
	http.setConnectTimeout(8000);
	http.setTimeout(10000);
	const int status = http.GET();
	if (status != HTTP_CODE_OK)
	{
		http.end();
		return false;
	}
	JsonDocument document;
	const DeserializationError error = deserializeJson(document, http.getString());
	http.end();
	JsonObject current = document["current"].as<JsonObject>();
	if (error || current.isNull())
		return false;
	weatherTemperatureC = current["temperature_2m"] | weatherTemperatureC;
	weatherWindKmh = current["wind_speed_10m"] | weatherWindKmh;
	weatherCode = current["weather_code"] | weatherCode;
	weatherHasData = true;
	return true;
}

void maybeRefreshWeather()
{
	if (idleTheme == 0 || !WiFi.isConnected() || weatherFetchInProgress)
		return;
	if (lastWeatherFetchMs != 0 && millis() - lastWeatherFetchMs < WEATHER_REFRESH_INTERVAL_MS)
		return;
	weatherFetchInProgress = true;
	const bool refreshed = fetchWeatherNow();
	lastWeatherFetchMs = millis();
	weatherFetchInProgress = false;
	Serial.printf("[WEATHER] Refresh %s for %s\n",
		refreshed ? "complete" : "failed", weatherCity.c_str());
	if (refreshed && idleClockActive)
	{
		lastClockSecond = -1;
		drawIdleClockScreen();
	}
}

bool readLocalClock(tm &clockTime)
{
	const time_t now = time(nullptr);
	if (now < 1700000000)
		return false;
	localtime_r(&now, &clockTime);
	return true;
}

void noteUserInteraction()
{
	lastInteractionMs = millis();
	lastClockMinute = -1;
	if (idleClockActive)
	{
		idleClockActive = false;
		drawCurrentPage();
	}
}

String normalizeTopicPrefix(String value)
{
	value.trim();
	while (value.startsWith("/"))
		value.remove(0, 1);
	while (value.endsWith("/"))
		value.remove(value.length() - 1);
	return value;
}

bool isValidMqttHost(String value)
{
	value.trim();
	return value.length() > 0 && value.length() <= 96 && value.indexOf("://") < 0 &&
		value.indexOf('/') < 0 && value.indexOf(' ') < 0;
}

bool isValidTopicPrefix(const String &value)
{
	return value.length() > 0 && value.length() <= 96 && value.indexOf('#') < 0 &&
		value.indexOf('+') < 0 && value.indexOf(' ') < 0;
}

bool isMqttConfigurationComplete()
{
	return isValidMqttHost(mqttHost) && mqttPort > 0 && mqttUsername.length() > 0 &&
		mqttPassword.length() > 0 && isValidTopicPrefix(mqttTopicPrefix);
}

void rebuildMqttTopics()
{
	const String root = normalizeTopicPrefix(mqttTopicPrefix);
	mqttMessageTopic = root + "/" + deviceId + "/messages";
	mqttCommandTopic = root + "/" + deviceId + "/cmd";
	mqttStatusTopic = root + "/" + deviceId + "/status";
	String legacyId = deviceId;
	legacyId.toUpperCase();
	mqttLegacyMessageTopic = root + "/" + legacyId + "/messages";
	mqttLegacyCommandTopic = root + "/" + legacyId + "/cmd";
}

void loadConfiguration()
{
	preferences.begin("wpnotif", true);
	deviceConfigured = preferences.getBool("configured", false);
	mqttHost = preferences.getString("mqtt_host", "");
	mqttPort = preferences.getUShort("mqtt_port", DEFAULT_MQTT_PORT);
	mqttUsername = preferences.getString("mqtt_user", "");
	mqttPassword = preferences.getString("mqtt_pass", "");
	mqttTopicPrefix = preferences.getString("mqtt_topic", DEFAULT_MQTT_TOPIC_PREFIX);
	clockUtcOffsetMinutes = preferences.getShort("clock_offset", 0);
	idleTheme = min<uint8_t>(preferences.getUChar("idle_theme", 0), 2);
	weatherCity = preferences.getString("wx_city", "Athens");
	weatherTimezone = preferences.getString("wx_tz", "");
	weatherLatitude = preferences.getFloat("wx_lat", 0.0F);
	weatherLongitude = preferences.getFloat("wx_lon", 0.0F);
	weatherHasCoordinates = preferences.getBool("wx_coords", false);
	screenBrightnessPercent = constrain(
		preferences.getUChar("brightness", DEFAULT_SCREEN_BRIGHTNESS), 10, 100);
	soundVolumePercent = constrain(
		preferences.getUChar("volume", DEFAULT_SOUND_VOLUME), 0, 100);
	preferences.end();
	mqttHost.trim();
	mqttUsername.trim();
	mqttTopicPrefix = normalizeTopicPrefix(mqttTopicPrefix);
	mqttConfigValid = isMqttConfigurationComplete();
	rebuildMqttTopics();
}

void syncPortalFields()
{
	safeCopy(mqttHostField, sizeof(mqttHostField), mqttHost);
	snprintf(mqttPortField, sizeof(mqttPortField), "%u", mqttPort);
	safeCopy(mqttUsernameField, sizeof(mqttUsernameField), mqttUsername);
	mqttPasswordField[0] = '\0';
	safeCopy(mqttTopicField, sizeof(mqttTopicField), mqttTopicPrefix);
	snprintf(clockOffsetField, sizeof(clockOffsetField), "%d", clockUtcOffsetMinutes);
	mqttHostParameter.setValue(mqttHostField, sizeof(mqttHostField));
	mqttPortParameter.setValue(mqttPortField, sizeof(mqttPortField));
	mqttUsernameParameter.setValue(mqttUsernameField, sizeof(mqttUsernameField));
	mqttPasswordParameter.setValue("", sizeof(mqttPasswordField));
	mqttTopicParameter.setValue(mqttTopicField, sizeof(mqttTopicField));
	clockOffsetParameter.setValue(clockOffsetField, sizeof(clockOffsetField));
}

bool savePortalConfiguration()
{
	String newHost = mqttHostParameter.getValue();
	String newUsername = mqttUsernameParameter.getValue();
	String newPassword = mqttPasswordParameter.getValue();
	String newTopic = normalizeTopicPrefix(String(mqttTopicParameter.getValue()));
	String portValue = mqttPortParameter.getValue();
	String offsetValue = clockOffsetParameter.getValue();
	newHost.trim();
	newUsername.trim();
	newPassword.trim();
	portValue.trim();
	offsetValue.trim();
	const long newPort = portValue.toInt();
	const long newClockOffset = offsetValue.toInt();

	if (!isValidMqttHost(newHost) || newPort < 1 || newPort > 65535 || !newUsername.length() ||
		(!newPassword.length() && !mqttPassword.length()) || !isValidTopicPrefix(newTopic) ||
		newClockOffset < -720 || newClockOffset > 840)
	{
		mqttConfigValid = false;
		return false;
	}

	mqttHost = newHost;
	mqttPort = static_cast<uint16_t>(newPort);
	mqttUsername = newUsername;
	if (newPassword.length())
		mqttPassword = newPassword;
	mqttTopicPrefix = newTopic;
	clockUtcOffsetMinutes = static_cast<int16_t>(newClockOffset);
	clockSyncStarted = false;
	mqttConfigValid = true;
	deviceConfigured = true;
	rebuildMqttTopics();

	preferences.begin("wpnotif", false);
	preferences.putBool("configured", true);
	preferences.putString("mqtt_host", mqttHost);
	preferences.putUShort("mqtt_port", mqttPort);
	preferences.putString("mqtt_user", mqttUsername);
	preferences.putString("mqtt_pass", mqttPassword);
	preferences.putString("mqtt_topic", mqttTopicPrefix);
	preferences.putShort("clock_offset", clockUtcOffsetMinutes);
	preferences.end();
	return true;
}

void saveDisplayConfiguration()
{
	preferences.begin("wpnotif", false);
	preferences.putUChar("idle_theme", idleTheme);
	preferences.putString("wx_city", weatherCity);
	preferences.putString("wx_tz", weatherTimezone);
	preferences.putFloat("wx_lat", weatherLatitude);
	preferences.putFloat("wx_lon", weatherLongitude);
	preferences.putBool("wx_coords", weatherHasCoordinates);
	preferences.putUChar("brightness", screenBrightnessPercent);
	preferences.putUChar("volume", soundVolumePercent);
	preferences.end();
}

void configureSetupPortal()
{
	portalHeadHtml = NOTIFICATOR_TOUCH_PORTAL_HEAD;
	portalHeadHtml.replace("%DEVICE_ID%", pairingId());
	deviceIdentityHtml = "<section class=\"device-identity\"><span>PAIRING ID</span><strong id=\"pairing-id\">" +
		pairingId() + "</strong><small>Use this ID when adding the device in the Notificator mobile app.</small></section>";
	deviceIdentityParameter = new WiFiManagerParameter(deviceIdentityHtml.c_str());

	wifiManager.setTitle("Notificator setup");
	wifiManager.setDarkMode(false);
	wifiManager.setCustomHeadElement(portalHeadHtml.c_str());
	wifiManager.setShowStaticFields(true);
	wifiManager.setShowDnsFields(false);
	wifiManager.setConfigPortalBlocking(false);
	wifiManager.addParameter(deviceIdentityParameter);
	wifiManager.addParameter(&mqttSectionParameter);
	wifiManager.addParameter(&mqttHostParameter);
	wifiManager.addParameter(&mqttPortParameter);
	wifiManager.addParameter(&mqttUsernameParameter);
	wifiManager.addParameter(&mqttPasswordParameter);
	wifiManager.addParameter(&mqttTopicParameter);
	wifiManager.addParameter(&clockOffsetParameter);
	wifiManager.addParameter(&mqttSectionEndParameter);
	wifiManager.setSaveParamsCallback([]()
		{ portalSaveRequested = true; });
}

void startSetupPortal()
{
	if (portalRunning)
		return;
	mqttClient.disconnect();
	syncPortalFields();
	portalSaveRequested = false;
	// Leave the current station before opening setup. Otherwise an existing
	// connection could be mistaken for a successful connection to newly saved
	// Wi-Fi credentials. NVS credentials are retained by the second argument.
	WiFi.disconnect(true, false);
	delay(120);
	WiFi.mode(WIFI_AP_STA);
	portalRunning = true;
	drawSetupScreen("OPEN SETUP ON YOUR PHONE");
	wifiManager.startConfigPortal(setupSsid.c_str());
	Serial.printf("[SETUP] Portal active: %s at 192.168.4.1\n", setupSsid.c_str());
}

void handleDeviceCommand(const String &json)
{
	JsonDocument document;
	if (deserializeJson(document, json) != DeserializationError::Ok || !document.is<JsonObject>())
		return;
	const String command = document["cmd"] | "";
	if (command == "idle_theme")
	{
		const int value = document["value"] | -1;
		if (value < 0 || value > 2)
			return;
		idleTheme = static_cast<uint8_t>(value);
		saveDisplayConfiguration();
		lastWeatherFetchMs = 0;
		lastClockSecond = -1;
		if (idleClockActive)
			drawIdleClockScreen();
		Serial.printf("[DISPLAY] Idle theme changed to %u\n", idleTheme);
		return;
	}
	if (command == "screen_brightness")
	{
		const int value = document["value"] | -1;
		if (value < 0 || value > 100)
			return;
		// Keep a small visible floor so a remote command cannot make the on-device
		// recovery controls unusable.
		screenBrightnessPercent = static_cast<uint8_t>(max(10, value));
		applyScreenBrightness();
		saveDisplayConfiguration();
		if (!idleClockActive && !wifiWizardActive)
			renderLvglPage(currentPage);
		return;
	}
	if (command == "sound_volume")
	{
		const int value = document["value"] | -1;
		if (value < 0 || value > 100)
			return;
		soundVolumePercent = static_cast<uint8_t>(value);
		applySoundVolume();
		saveDisplayConfiguration();
		if (!idleClockActive && !wifiWizardActive)
			renderLvglPage(currentPage);
		return;
	}
	if (command == "clear_msgs")
	{
		alertCount = 0;
		selectedAlertIndex = 0;
		if (!wifiWizardActive)
			drawCurrentPage();
		return;
	}
	if (command == "weather_config")
	{
		const bool hasLat = !document["lat"].isNull() || !document["latitude"].isNull();
		const bool hasLon = !document["lon"].isNull() || !document["longitude"].isNull();
		if (hasLat != hasLon)
			return;
		if (hasLat)
		{
			const float latitude = !document["lat"].isNull()
				? document["lat"].as<float>() : document["latitude"].as<float>();
			const float longitude = !document["lon"].isNull()
				? document["lon"].as<float>() : document["longitude"].as<float>();
			if (latitude < -90.0F || latitude > 90.0F || longitude < -180.0F || longitude > 180.0F)
				return;
			weatherLatitude = latitude;
			weatherLongitude = longitude;
			weatherHasCoordinates = true;
		}
		String city = document["city"] | document["location"] | "";
		city.trim();
		if (city.length())
		{
			if (city != weatherCity && !hasLat)
				weatherHasCoordinates = false;
			weatherCity = city.substring(0, 64);
		}
		String timezone = document["timezone"] | document["tz"] | "";
		timezone.trim();
		if (timezone.length())
		{
			weatherTimezone = timezone.substring(0, 64);
			clockSyncStarted = false;
			startClockSync();
		}
		weatherHasData = false;
		lastWeatherFetchMs = 0;
		saveDisplayConfiguration();
		Serial.printf("[WEATHER] Configuration updated for %s\n", weatherCity.c_str());
		return;
	}
	if (command == "ota")
	{
		const String channel = document["channel"] | NOTIFICATOR_OTA_DEFAULT_CHANNEL;
		const bool force = document["force"] | false;
		performOfficialOtaUpdate(channel, force);
	}
}

void handleIncomingMqtt(char *topic, uint8_t *payload, unsigned int length)
{
	const String receivedTopic = String(topic ? topic : "");
	const bool commandMessage = receivedTopic == mqttCommandTopic ||
		receivedTopic == mqttLegacyCommandTopic;
	if (!commandMessage && receivedTopic != mqttMessageTopic && receivedTopic != mqttLegacyMessageTopic)
		return;
	String raw;
	raw.reserve(length + 1);
	for (unsigned int index = 0; index < length; ++index)
		raw += static_cast<char>(payload[index]);
	if (commandMessage)
	{
		handleDeviceCommand(raw);
		return;
	}

	JsonDocument document;
	const DeserializationError error = deserializeJson(document, raw);
	if (!error && document.is<JsonObject>())
	{
		addAlert(
			String(document["title"] | "NEW NOTIFICATION"),
			String(document["body"] | document["message"] | "OPEN THE APP FOR DETAILS"),
			String(document["site"] | document["source"] | "NOTIFICATOR"),
			String(document["severity"] | "info"));
	}
	else
	{
		addAlert("NEW NOTIFICATION", raw, "NOTIFICATOR");
	}
	idleClockActive = false;
	lastInteractionMs = millis();
	currentPage = 1;
	if (!wifiWizardActive)
		drawCurrentPage();
	playTestChime();
	Serial.printf("[MQTT] Notification received on %s\n", mqttMessageTopic.c_str());
}

void configureMqttClient()
{
	if (!mqttConfigValid)
		return;
	mqttTlsClient.setCACert(MQTT_CA_CERT);
	mqttClient.setServer(mqttHost.c_str(), mqttPort);
	mqttClient.setBufferSize(1024);
	mqttClient.setCallback(handleIncomingMqtt);
}

bool publishDeviceStatus(
	const char *eventName = "online",
	const char *otaStatus = nullptr,
	const String &targetVersion = "",
	const String &error = "")
{
	if (!mqttClient.connected())
		return false;
	JsonDocument document;
	document["type"] = "device_telemetry";
	document["event"] = eventName;
	document["deviceId"] = deviceId;
	document["firmwareVersion"] = FIRMWARE_VERSION;
	document["model"] = MODEL_ID;
	document["uptime"] = millis() / 1000;
	document["freeHeap"] = ESP.getFreeHeap();
	document["rssi"] = WiFi.RSSI();
	document["status"] = "ready";
	if (otaStatus && otaStatus[0])
		document["otaStatus"] = otaStatus;
	if (targetVersion.length())
		document["targetVersion"] = targetVersion;
	if (error.length())
		document["error"] = error;
	if (batteryAvailable)
	{
		document["batteryPercent"] = batteryPercent;
		document["batteryVoltage"] = roundf(batteryVoltage * 100.0F) / 100.0F;
	}
	String payload;
	serializeJson(document, payload);
	lastMqttStatusMs = millis();
	return mqttClient.publish(mqttStatusTopic.c_str(), payload.c_str(), true);
}

void drawOtaStatus(const String &title, const String &detail, int percent = -1)
{
	idleClockActive = false;
	fillRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BACKGROUND);
	drawCenteredText(SCREEN_WIDTH / 2, 28, title, 4, COLOR_WHITE, COLOR_BACKGROUND);
	drawCenteredText(SCREEN_WIDTH / 2, 78, detail, 2, COLOR_MUTED, COLOR_BACKGROUND);
	if (percent >= 0)
	{
		fillRoundedRectangle(70, 122, 500, 18, 9, COLOR_PANEL_RAISED);
		fillRoundedRectangle(70, 122, max(10, percent * 5), 18, 9, COLOR_BLUE);
		drawCenteredText(SCREEN_WIDTH / 2, 148, String(percent) + "%", 2,
			COLOR_BLUE_LIGHT, COLOR_BACKGROUND);
	}
	presentDisplay();
}

String normalizeOtaChannel(String channel)
{
	channel.trim();
	channel.toLowerCase();
	return channel == "preview" ? "preview" : String(NOTIFICATOR_OTA_DEFAULT_CHANNEL);
}

bool isSha256Hex(const String &value)
{
	if (value.length() != 64)
		return false;
	for (size_t index = 0; index < value.length(); ++index)
		if (!((value[index] >= '0' && value[index] <= '9') ||
			(value[index] >= 'a' && value[index] <= 'f')))
			return false;
	return true;
}

String sha256ToHex(const unsigned char digest[32])
{
	static const char alphabet[] = "0123456789abcdef";
	char output[65];
	for (size_t index = 0; index < 32; ++index)
	{
		output[index * 2] = alphabet[(digest[index] >> 4) & 0x0f];
		output[index * 2 + 1] = alphabet[digest[index] & 0x0f];
	}
	output[64] = '\0';
	return String(output);
}

bool fetchOfficialOtaRelease(const String &requestedChannel, OtaRelease &release, String &error)
{
	const String channel = normalizeOtaChannel(requestedChannel);
	WiFiClientSecure client;
	client.setCACert(MQTT_CA_CERT);
	client.setTimeout(12000);
	HTTPClient http;
	http.setConnectTimeout(10000);
	http.setTimeout(12000);
	http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	if (!http.begin(client, NOTIFICATOR_OTA_MANIFEST_URL))
	{
		error = "manifest_begin";
		return false;
	}
	const int status = http.GET();
	if (status != HTTP_CODE_OK)
	{
		error = "manifest_http_" + String(status);
		http.end();
		return false;
	}
	const String body = http.getString();
	http.end();
	if (!body.length() || body.length() > 16384)
	{
		error = "manifest_size";
		return false;
	}
	JsonDocument document;
	if (deserializeJson(document, body) != DeserializationError::Ok)
	{
		error = "manifest_json";
		return false;
	}
	JsonObject entry = document["channels"][channel]["deviceTypes"]
		[NOTIFICATOR_OTA_DEVICE_TYPE].as<JsonObject>();
	if ((document["schemaVersion"] | 0) != 2 || entry.isNull())
	{
		error = "manifest_model";
		return false;
	}
	release.channel = channel;
	release.deviceType = entry["deviceType"] | "";
	release.board = entry["board"] | "";
	release.version = entry["version"] | "";
	release.url = entry["url"] | "";
	release.sha256 = entry["sha256"] | "";
	release.size = entry["size"] | 0;
	release.releasedAt = entry["releasedAt"] | "";
	release.signature = entry["signature"] | "";
	release.keyId = entry["keyId"] | "";
	release.sha256.toLowerCase();
	int major = 0;
	int minor = 0;
	int patch = 0;
	const String algorithm = entry["signatureAlgorithm"] | "";
	if (release.deviceType != NOTIFICATOR_OTA_DEVICE_TYPE ||
		release.board != NOTIFICATOR_OTA_BOARD ||
		!parseVersionTriplet(release.version, major, minor, patch) ||
		!isValidOtaUrl(release.url) || !isSha256Hex(release.sha256) ||
		release.size == 0 || release.size > 3145728 || !release.releasedAt.length() ||
		release.keyId != NOTIFICATOR_OTA_KEY_ID || algorithm != "ECDSA-P256-SHA256")
	{
		error = "manifest_fields";
		return false;
	}
	const String signedPayload = buildOtaReleaseSignBase(
		release.channel, release.deviceType, release.board, release.version,
		release.url, release.sha256, release.size, release.releasedAt);
	if (!verifyOtaReleaseSignature(
			NOTIFICATOR_OTA_PUBLIC_KEY_PEM, signedPayload, release.signature))
	{
		error = "manifest_signature";
		return false;
	}
	return true;
}

bool streamVerifiedOtaImage(const OtaRelease &release, String &error)
{
	WiFiClientSecure client;
	client.setCACert(MQTT_CA_CERT);
	client.setTimeout(12000);
	HTTPClient http;
	http.setConnectTimeout(10000);
	http.setTimeout(12000);
	http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	if (!http.begin(client, release.url))
	{
		error = "binary_begin";
		return false;
	}
	const int status = http.GET();
	if (status != HTTP_CODE_OK)
	{
		error = "binary_http_" + String(status);
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
	mbedtls_sha256_context sha;
	mbedtls_sha256_init(&sha);
	if (mbedtls_sha256_starts(&sha, 0) != 0)
	{
		error = "sha_begin";
		mbedtls_sha256_free(&sha);
		Update.abort();
		http.end();
		return false;
	}
	WiFiClient *stream = http.getStreamPtr();
	unsigned char buffer[2048];
	size_t received = 0;
	unsigned long lastDataAt = millis();
	int lastPercent = -1;
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
		const size_t wanted = min(
			release.size - received,
			static_cast<size_t>(min(available, static_cast<int>(sizeof(buffer)))));
		const size_t count = stream->readBytes(buffer, wanted);
		if (!count)
			continue;
		lastDataAt = millis();
		if (mbedtls_sha256_update(&sha, buffer, count) != 0 ||
			Update.write(buffer, count) != count)
		{
			error = "binary_write";
			break;
		}
		received += count;
		const int percent = static_cast<int>((received * 100ULL) / release.size);
		if (percent != lastPercent && (percent % 2 == 0 || percent == 100))
		{
			lastPercent = percent;
			drawOtaStatus("UPDATING", release.version, percent);
		}
	}
	unsigned char digest[32];
	const bool digestReady = !error.length() && received == release.size &&
		mbedtls_sha256_finish(&sha, digest) == 0;
	mbedtls_sha256_free(&sha);
	if (!digestReady || !otaSecureHexEquals(release.sha256, sha256ToHex(digest)))
	{
		if (!error.length())
			error = digestReady ? "binary_hash" : "binary_incomplete";
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

void performOfficialOtaUpdate(const String &requestedChannel, bool force)
{
	if (!WiFi.isConnected())
		return;
	drawOtaStatus("FIRMWARE", "CHECKING RELEASE");
	OtaRelease release;
	String error;
	if (!fetchOfficialOtaRelease(requestedChannel, release, error))
	{
		publishDeviceStatus("ota_result", "failed", "", error);
		drawOtaStatus("UPDATE FAILED", error);
		delay(1600);
		drawCurrentPage();
		return;
	}
	if (!force && !isRemoteVersionNewer(FIRMWARE_VERSION, release.version))
	{
		publishDeviceStatus("ota_result", "no_update", release.version);
		drawOtaStatus("UP TO DATE", FIRMWARE_VERSION);
		delay(1300);
		drawCurrentPage();
		return;
	}
	publishDeviceStatus("ota_result", "updating", release.version);
	drawOtaStatus("FIRMWARE", "STARTING", 0);
	if (!streamVerifiedOtaImage(release, error))
	{
		publishDeviceStatus("ota_result", "failed", release.version, error);
		drawOtaStatus("UPDATE FAILED", error);
		delay(1800);
		drawCurrentPage();
		return;
	}
	publishDeviceStatus("ota_result", "restarting", release.version);
	drawOtaStatus("UPDATE READY", "RESTARTING", 100);
	delay(1200);
	ESP.restart();
}

void connectMqtt()
{
	if (mqttClient.connected() || !WiFi.isConnected() || !mqttConfigValid)
		return;
	const String clientId = "notificator-touch-" + deviceId;
	if (mqttClient.connect(clientId.c_str(), mqttUsername.c_str(), mqttPassword.c_str()))
	{
		lastMqttHealthMs = millis();
		mqttClient.subscribe(mqttMessageTopic.c_str(), 1);
		mqttClient.subscribe(mqttCommandTopic.c_str(), 1);
		if (mqttLegacyMessageTopic != mqttMessageTopic)
		{
			// Older app records may contain the uppercase ID previously shown on
			// the Touch setup screen. MQTT is case-sensitive, so listen to both
			// forms until every client normalizes IDs before publishing.
			mqttClient.subscribe(mqttLegacyMessageTopic.c_str(), 1);
			mqttClient.subscribe(mqttLegacyCommandTopic.c_str(), 1);
		}
		publishDeviceStatus();
		networkStateChanged = true;
		Serial.printf("[MQTT] Connected and listening on %s\n", mqttMessageTopic.c_str());
	}
	else
	{
		Serial.printf("[MQTT] Connection failed, state=%d\n", mqttClient.state());
	}
}

bool probeAddress(TwoWire &bus, uint8_t address)
{
	bus.beginTransmission(address);
	return bus.endTransmission() == 0;
}

bool readSystemRegister(uint8_t address, uint8_t reg, uint8_t &value)
{
	Wire.beginTransmission(address);
	Wire.write(reg);
	if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, static_cast<uint8_t>(1)) != 1)
	{
		return false;
	}
	value = Wire.read();
	return true;
}

bool writeSystemRegister(uint8_t address, uint8_t reg, uint8_t value)
{
	Wire.beginTransmission(address);
	Wire.write(reg);
	Wire.write(value);
	return Wire.endTransmission() == 0;
}

bool readSystemRegisters(uint8_t address, uint8_t firstRegister, uint8_t *values, size_t length)
{
	Wire.beginTransmission(address);
	Wire.write(firstRegister);
	if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, static_cast<uint8_t>(length)) != length)
	{
		return false;
	}
	for (size_t index = 0; index < length; ++index)
	{
		values[index] = Wire.read();
	}
	return true;
}

bool readAcceleration(int16_t &x, int16_t &y, int16_t &z)
{
	uint8_t values[6] = {};
	if (!readSystemRegisters(IMU_ADDRESS, IMU_REGISTER_ACCEL_X_LOW, values, sizeof(values)))
	{
		return false;
	}
	x = static_cast<int16_t>((static_cast<uint16_t>(values[1]) << 8) | values[0]);
	y = static_cast<int16_t>((static_cast<uint16_t>(values[3]) << 8) | values[2]);
	z = static_cast<int16_t>((static_cast<uint16_t>(values[5]) << 8) | values[4]);
	return true;
}

bool initializeImuRotation()
{
	if (!probeAddress(Wire, IMU_ADDRESS) ||
		!writeSystemRegister(IMU_ADDRESS, IMU_REGISTER_RESET, 0xB0))
	{
		return false;
	}

	delay(25);
	uint8_t resetResult = 0;
	if (!readSystemRegister(IMU_ADDRESS, IMU_REGISTER_RESET_RESULT, resetResult) || resetResult != 0x80)
	{
		Serial.printf("[IMU] Reset result was 0x%02X\n", resetResult);
		return false;
	}

	// Enable register auto-increment, configure the accelerometer for +/-4 g at
	// 31.25 Hz with its low-pass filter, then enable only the accelerometer.
	if (!writeSystemRegister(IMU_ADDRESS, IMU_REGISTER_CTRL1, 0x40) ||
		!writeSystemRegister(IMU_ADDRESS, IMU_REGISTER_CTRL2, 0x18) ||
		!writeSystemRegister(IMU_ADDRESS, IMU_REGISTER_CTRL5, 0x01) ||
		!writeSystemRegister(IMU_ADDRESS, IMU_REGISTER_CTRL7, 0x01))
	{
		return false;
	}
	delay(120);

	int32_t xTotal = 0;
	int32_t yTotal = 0;
	int validSamples = 0;
	for (int sample = 0; sample < 12; ++sample)
	{
		int16_t x = 0;
		int16_t y = 0;
		int16_t z = 0;
		if (readAcceleration(x, y, z))
		{
			xTotal += x;
			yTotal += y;
			++validSamples;
		}
		delay(20);
	}
	if (validSamples == 0)
	{
		return false;
	}

	const int averagedX = xTotal / validSamples;
	const int averagedY = yTotal / validSamples;
	orientationAxis = abs(averagedX) > abs(averagedY) ? 0 : 1;
	const int referenceValue = orientationAxis == 0 ? averagedX : averagedY;
	if (abs(referenceValue) >= IMU_FLIP_THRESHOLD)
	{
		orientationReferenceSign = referenceValue >= 0 ? 1 : -1;
		orientationCalibrated = true;
		Serial.printf("[IMU] Auto-rotation ready axis=%c reference=%d\n",
			orientationAxis == 0 ? 'X' : 'Y', referenceValue);
	}
	else
	{
		Serial.printf("[IMU] Waiting for upright calibration x=%d y=%d\n", averagedX, averagedY);
	}
	return true;
}

void updateOrientation()
{
	if (!imuRotationReady || millis() - lastImuSampleMs < 80)
	{
		return;
	}
	lastImuSampleMs = millis();

	int16_t x = 0;
	int16_t y = 0;
	int16_t z = 0;
	if (!readAcceleration(x, y, z))
	{
		return;
	}
	if (!orientationCalibrated)
	{
		orientationAxis = abs(x) > abs(y) ? 0 : 1;
		const int calibrationValue = orientationAxis == 0 ? x : y;
		if (abs(calibrationValue) >= IMU_FLIP_THRESHOLD)
		{
			orientationReferenceSign = calibrationValue >= 0 ? 1 : -1;
			orientationCalibrated = true;
			drawCurrentPage();
			Serial.printf("[IMU] Upright calibration complete axis=%c reference=%d\n",
				orientationAxis == 0 ? 'X' : 'Y', calibrationValue);
		}
		return;
	}
	const int axisValue = orientationAxis == 0 ? x : y;
	if (abs(axisValue) < IMU_FLIP_THRESHOLD)
	{
		orientationCandidateSinceMs = 0;
		return;
	}

	const bool desiredFlip = (axisValue >= 0 ? 1 : -1) != orientationReferenceSign;
	if (desiredFlip == displayFlipped)
	{
		orientationCandidateSinceMs = 0;
		return;
	}
	if (orientationCandidateSinceMs == 0 || orientationCandidate != desiredFlip)
	{
		orientationCandidate = desiredFlip;
		orientationCandidateSinceMs = millis();
		return;
	}
	if (millis() - orientationCandidateSinceMs < IMU_FLIP_STABILITY_MS)
	{
		return;
	}

	orientationCandidateSinceMs = 0;
	touchActive = false;
	missedTouchReads = 0;
	animateOrientationChange(desiredFlip);
	Serial.printf("[IMU] Display orientation: %s\n", desiredFlip ? "flipped" : "normal");
}

bool setIoExpanderOutput(uint8_t bit, bool high)
{
	// Preserve unrelated TCA9554 bits because the expander also controls the
	// audio amplifier, display peripherals, and battery-power latch.
	uint8_t output = 0;
	uint8_t direction = 0;
	if (!readSystemRegister(IO_EXPANDER_ADDRESS, 0x01, output) ||
		!readSystemRegister(IO_EXPANDER_ADDRESS, 0x03, direction))
	{
		return false;
	}

	const uint8_t mask = static_cast<uint8_t>(1U << bit);
	direction &= static_cast<uint8_t>(~mask);
	if (high)
	{
		output |= mask;
	}
	else
	{
		output &= static_cast<uint8_t>(~mask);
	}
	return writeSystemRegister(IO_EXPANDER_ADDRESS, 0x03, direction) &&
		writeSystemRegister(IO_EXPANDER_ADDRESS, 0x01, output);
}

bool enableAudioPower()
{
	// TCA9554 pin 7 enables the external speaker/amplifier path.
	return setIoExpanderOutput(7, true);
}

bool holdSystemPower()
{
	// TCA9554 pin 6 keeps battery-powered operation latched after PWR is
	// released. USB power can independently keep the board running.
	return setIoExpanderOutput(6, true);
}

bool releaseSystemPower()
{
	return setIoExpanderOutput(6, false);
}

bool initializeAudio()
{
	if (!probeAddress(Wire, AUDIO_CODEC_ADDRESS) || !enableAudioPower())
	{
		Serial.println("[AUDIO] Codec or amplifier power control was not found");
		return false;
	}

	set_codec_board_type("S3_LCD_3_49");
	codec_init_cfg_t codecConfig = {};
	codecConfig.in_mode = CODEC_I2S_MODE_NONE;
	codecConfig.out_mode = CODEC_I2S_MODE_TDM;
	codecConfig.in_use_tdm = false;
	codecConfig.reuse_dev = false;
	if (init_codec(&codecConfig) != ESP_OK)
	{
		Serial.println("[AUDIO] Codec initialization failed");
		return false;
	}

	audioPlayback = get_playback_handle();
	if (audioPlayback == nullptr)
	{
		Serial.println("[AUDIO] Playback handle was not created");
		return false;
	}

	esp_codec_dev_sample_info_t sampleInfo = {};
	sampleInfo.sample_rate = AUDIO_SAMPLE_RATE;
	sampleInfo.channel = 2;
	sampleInfo.bits_per_sample = 16;
	if (esp_codec_dev_open(audioPlayback, &sampleInfo) != ESP_CODEC_DEV_OK)
	{
		Serial.println("[AUDIO] Playback stream could not be opened");
		return false;
	}
	esp_codec_dev_set_out_vol(audioPlayback, 68.0F);
	Serial.println("[AUDIO] ES8311 playback ready");
	return true;
}

bool playTestChime()
{
	if (!audioReady || audioPlayback == nullptr)
	{
		return false;
	}

	constexpr int durationMs = 360;
	constexpr int frameCount = AUDIO_SAMPLE_RATE * durationMs / 1000;
	auto *samples = static_cast<int16_t *>(heap_caps_malloc(
		static_cast<size_t>(frameCount) * 2 * sizeof(int16_t),
		MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
	if (samples == nullptr)
	{
		return false;
	}

	for (int frame = 0; frame < frameCount; ++frame)
	{
		const float progress = static_cast<float>(frame) / frameCount;
		const float frequency = progress < 0.5F ? 660.0F : 880.0F;
		const float envelope = min(1.0F, progress * 16.0F) * min(1.0F, (1.0F - progress) * 12.0F);
		const int16_t sample = static_cast<int16_t>(sinf(2.0F * PI * frequency * frame / AUDIO_SAMPLE_RATE) * 10500.0F * envelope);
		samples[frame * 2] = sample;
		samples[frame * 2 + 1] = sample;
	}

	const int result = esp_codec_dev_write(
		audioPlayback,
		samples,
		static_cast<int>(frameCount * 2 * sizeof(int16_t)));
	heap_caps_free(samples);
	Serial.printf("[AUDIO] Test chime result=%d\n", result);
	return result == ESP_CODEC_DEV_OK;
}

int scanBus(TwoWire &bus, const char *name)
{
	int count = 0;
	Serial.printf("[I2C] Scanning %s bus\n", name);
	for (uint8_t address = 1; address < 127; ++address)
	{
		if (probeAddress(bus, address))
		{
			Serial.printf("[I2C] %s device at 0x%02X\n", name, address);
			++count;
		}
	}
	return count;
}

bool testPsram()
{
	constexpr size_t TEST_BYTES = 64 * 1024;
	if (!psramFound() || esp_psram_get_size() < TEST_BYTES)
	{
		return false;
	}

	auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(TEST_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
	if (buffer == nullptr)
	{
		return false;
	}

	for (size_t index = 0; index < TEST_BYTES; ++index)
	{
		buffer[index] = static_cast<uint8_t>((index * 37U) ^ 0xA5U);
	}

	bool valid = true;
	for (size_t index = 0; index < TEST_BYTES; ++index)
	{
		if (buffer[index] != static_cast<uint8_t>((index * 37U) ^ 0xA5U))
		{
			valid = false;
			break;
		}
	}

	heap_caps_free(buffer);
	return valid;
}

bool initializeDisplay()
{
	transferComplete = xSemaphoreCreateBinary();
	if (transferComplete == nullptr)
	{
		return false;
	}

	transferBuffer = static_cast<uint16_t *>(heap_caps_malloc(
		LCD_NATIVE_WIDTH * LCD_CHUNK_HEIGHT * sizeof(uint16_t),
		MALLOC_CAP_DMA));
	if (transferBuffer == nullptr)
	{
		return false;
	}
	frameBuffer = static_cast<uint16_t *>(heap_caps_malloc(
		static_cast<size_t>(SCREEN_WIDTH) * SCREEN_HEIGHT * sizeof(uint16_t),
		MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
	if (frameBuffer == nullptr)
	{
		return false;
	}

	gpio_config_t resetConfig = {};
	resetConfig.mode = GPIO_MODE_OUTPUT;
	resetConfig.pin_bit_mask = 1ULL << LCD_PIN_RESET;
	resetConfig.pull_up_en = GPIO_PULLUP_ENABLE;
	ESP_ERROR_CHECK(gpio_config(&resetConfig));

	spi_bus_config_t busConfig = {};
	busConfig.data0_io_num = LCD_PIN_DATA_0;
	busConfig.data1_io_num = LCD_PIN_DATA_1;
	busConfig.data2_io_num = LCD_PIN_DATA_2;
	busConfig.data3_io_num = LCD_PIN_DATA_3;
	busConfig.sclk_io_num = LCD_PIN_CLOCK;
	busConfig.max_transfer_sz = LCD_NATIVE_WIDTH * LCD_CHUNK_HEIGHT * sizeof(uint16_t);
	ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &busConfig, SPI_DMA_CH_AUTO));

	esp_lcd_panel_io_spi_config_t ioConfig = AXS15231B_PANEL_IO_QSPI_CONFIG(
		LCD_PIN_CS,
		onColorTransferComplete,
		nullptr);
	esp_lcd_panel_io_handle_t panelIo = nullptr;
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &ioConfig, &panelIo));

	static const axs15231b_lcd_init_cmd_t initializationCommands[] = {
		{0x11, static_cast<const uint8_t *>(nullptr), 0, 100},
		{0x29, static_cast<const uint8_t *>(nullptr), 0, 100},
	};

	axs15231b_vendor_config_t vendorConfig = {};
	vendorConfig.flags.use_qspi_interface = 1;
	vendorConfig.init_cmds = initializationCommands;
	vendorConfig.init_cmds_size = sizeof(initializationCommands) / sizeof(initializationCommands[0]);

	esp_lcd_panel_dev_config_t panelConfig = {};
	panelConfig.reset_gpio_num = -1;
	panelConfig.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
	panelConfig.bits_per_pixel = 16;
	panelConfig.vendor_config = &vendorConfig;

	ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panelIo, &panelConfig, &lcdPanel));
	gpio_set_level(LCD_PIN_RESET, 1);
	delay(30);
	gpio_set_level(LCD_PIN_RESET, 0);
	delay(250);
	gpio_set_level(LCD_PIN_RESET, 1);
	delay(30);
	ESP_ERROR_CHECK(esp_lcd_panel_init(lcdPanel));

	displayReady = true;
	return true;
}

void pulseBacklight()
{
	pinMode(LCD_PIN_BACKLIGHT, OUTPUT);
	// Waveshare drives the backlight through an active-low transistor. A high
	// level briefly demonstrates the off state; low keeps full brightness on.
	digitalWrite(LCD_PIN_BACKLIGHT, HIGH);
	delay(180);
	digitalWrite(LCD_PIN_BACKLIGHT, LOW);
}

/** Drive the active-low LCD backlight with flicker-free PWM. */
void applyScreenBrightness()
{
	static bool pwmAttached = false;
	if (!pwmAttached)
	{
		pwmAttached = ledcAttach(LCD_PIN_BACKLIGHT, 20000, 8);
		if (!pwmAttached)
		{
			Serial.println("[DISPLAY] Backlight PWM could not be attached");
			return;
		}
	}

	const uint8_t activeLowDuty = static_cast<uint8_t>(
		(100 - screenBrightnessPercent) * 255 / 100);
	ledcWrite(LCD_PIN_BACKLIGHT, activeLowDuty);
	Serial.printf("[DISPLAY] Brightness set to %u%%\n", screenBrightnessPercent);
}

/** Apply the saved output level to the ES8311 playback path. */
void applySoundVolume()
{
	if (audioPlayback == nullptr)
		return;
	esp_codec_dev_set_out_vol(audioPlayback, static_cast<float>(soundVolumePercent));
	Serial.printf("[AUDIO] Volume set to %u%%\n", soundVolumePercent);
}

void drawBrandMark(int x, int y, int size, uint16_t background)
{
	fillRoundedRectangle(x, y, size, size, size / 4, COLOR_BLUE);
	drawOutline(x + size / 4, y + size / 4, size / 2, size / 2, COLOR_WHITE, 2);
	fillRectangle(x + size / 4, y + size * 3 / 4 - 2, size / 7, size / 7, COLOR_BLUE);
	(void)background;
}

void drawAppHeader(const String &section)
{
	fillRectangle(0, 0, SCREEN_WIDTH, 42, COLOR_PANEL);
	drawText(14, 10, "NOTIFICATOR", 2, COLOR_WHITE, COLOR_PANEL);
	drawText(174, 10, section, 2, COLOR_BLUE_LIGHT, COLOR_PANEL);

	const bool wifiReady = WiFi.isConnected();
	const bool mqttHealthy = mqttClient.connected() && millis() - lastMqttHealthMs <= MQTT_HEALTH_GRACE_MS;
	const bool mqttPulseVisible = (millis() / MQTT_PULSE_INTERVAL_MS) % 2 == 0;
	fillCircle(394, 21, 4, wifiReady ? COLOR_GREEN : COLOR_RED);
	drawText(405, 17, wifiReady ? "WIFI" : "OFFLINE", 1, COLOR_MUTED, COLOR_PANEL);
	fillCircle(468, 21, 4,
		mqttHealthy ? (mqttPulseVisible ? COLOR_GREEN : COLOR_PANEL) : COLOR_RED);
	drawText(479, 17, mqttHealthy ? "LIVE" : "LOST", 1,
		mqttHealthy ? COLOR_MUTED : COLOR_RED, COLOR_PANEL);

	const int batteryX = 550;
	drawOutline(batteryX, 14, 24, 14, batteryAvailable && batteryPercent <= 15 ? COLOR_RED : COLOR_MUTED, 2);
	fillRectangle(batteryX + 24, 18, 3, 6, COLOR_MUTED);
	if (batteryAvailable)
	{
		const int fillWidth = max(2, 18 * static_cast<int>(batteryPercent) / 100);
		fillRectangle(batteryX + 3, 17, fillWidth, 8,
			batteryPercent <= 15 ? COLOR_RED : batteryPercent <= 35 ? COLOR_AMBER : COLOR_GREEN);
		drawText(584, 17, String(batteryPercent) + "%", 1, COLOR_WHITE, COLOR_PANEL);
	}
	else
	{
		drawText(584, 17, "USB", 1, COLOR_MUTED, COLOR_PANEL);
	}
}

void drawBottomNavigation(uint8_t active)
{
	fillRectangle(0, 138, SCREEN_WIDTH, 34, COLOR_PANEL);
	const char *labels[] = {"HOME", "ALERTS", "DEVICE"};
	for (uint8_t index = 0; index < 3; ++index)
	{
		const int itemX = index * (SCREEN_WIDTH / 3);
		if (index == active)
			fillRoundedRectangle(itemX + 12, 141, SCREEN_WIDTH / 3 - 24, 28, 12, COLOR_BLUE);
		drawCenteredText(itemX + SCREEN_WIDTH / 6, 147, labels[index], 2,
			index == active ? COLOR_WHITE : COLOR_MUTED,
			index == active ? COLOR_BLUE : COLOR_PANEL);
		if (index == 1 && unreadAlertCount() > 0)
			fillCircle(itemX + SCREEN_WIDTH / 6 + 58, 145, 4, COLOR_BLUE_LIGHT);
	}
}

void drawBootSplash()
{
	if (lvglReady)
	{
		renderLvglSplash();
		return;
	}
	fillRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLUE);
	fillCircle(92, 86, 54, COLOR_BLUE_LIGHT);
	drawBrandMark(61, 55, 62, COLOR_BLUE);
	drawText(174, 52, "NOTIFICATOR", 3, COLOR_WHITE, COLOR_BLUE);
	drawText(176, 92, "TOUCH DISPLAY", 1, COLOR_WHITE, COLOR_BLUE);
	drawText(176, 116, "ALERTS YOU CAN SEE AND HEAR", 1, COLOR_WHITE, COLOR_BLUE);
	presentDisplay();
}

void drawSetupScreen(const String &state)
{
	if (lvglReady)
	{
		renderLvglSetup(state);
		return;
	}
	fillRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BACKGROUND);
	fillRoundedRectangle(10, 10, 194, 152, 16, COLOR_BLUE);
	drawBrandMark(26, 26, 34, COLOR_BLUE);
	drawText(72, 32, "SETUP", 2, COLOR_WHITE, COLOR_BLUE);
	drawText(26, 78, "PAIRING ID", 1, COLOR_WHITE, COLOR_BLUE);
	drawText(26, 98, displayPairingId(), 2, COLOR_WHITE, COLOR_BLUE);
	drawText(26, 134, "KEEP THIS ID FOR THE APP", 1, COLOR_WHITE, COLOR_BLUE);

	drawText(224, 18, "CONNECT YOUR DEVICE", 2, COLOR_WHITE, COLOR_BACKGROUND);
	drawText(224, 52, "1  JOIN THIS WIFI NETWORK", 1, COLOR_MUTED, COLOR_BACKGROUND);
	fillRoundedRectangle(224, 68, 390, 36, 10, COLOR_PANEL);
	drawText(238, 80, setupSsid, 1, COLOR_CYAN, COLOR_PANEL);
	drawText(224, 116, "2  OPEN 192.168.4.1 AND ADD WIFI PLUS HIVEMQ", 1, COLOR_MUTED, COLOR_BACKGROUND);
	drawText(224, 144, state, 1, COLOR_GREEN, COLOR_BACKGROUND);
	if (!deferPagePresentation)
		presentDisplay();
}

void drawDashboardScreen()
{
	if (lvglReady)
	{
		renderLvglPage(0);
		return;
	}
	fillRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BACKGROUND);
	drawAppHeader("HOME");

	fillRoundedRectangle(10, 50, 358, 86, 14, COLOR_BLUE);
	drawText(28, 61, mqttClient.connected() ? "READY" : "SETUP NEEDED", 3, COLOR_WHITE, COLOR_BLUE);
	drawText(28, 96, mqttClient.connected() ? "CONNECTED" : "CONNECT DEVICE", 2, COLOR_WHITE, COLOR_BLUE);
	drawText(28, 116, "ID " + displayPairingId(), 2, COLOR_WHITE, COLOR_BLUE);

	fillRoundedRectangle(378, 50, 252, 40, 12, COLOR_PANEL_RAISED);
	drawText(394, 59, "ALERTS", 2, COLOR_MUTED, COLOR_PANEL_RAISED);
	drawCenteredText(588, 55, String(unreadAlertCount()), 3,
		unreadAlertCount() > 0 ? COLOR_BLUE_LIGHT : COLOR_WHITE, COLOR_PANEL_RAISED);

	const uint16_t connectionColor = mqttClient.connected() ? COLOR_PANEL_RAISED : COLOR_BLUE;
	fillRoundedRectangle(378, 96, 252, 40, 12, connectionColor);
	drawText(394, 105, "MQTT", 2,
		mqttClient.connected() ? COLOR_MUTED : COLOR_WHITE, connectionColor);
	drawCenteredText(568, 105, mqttClient.connected() ? "LIVE" : "SETUP", 2,
		COLOR_WHITE, connectionColor);
	drawBottomNavigation(0);
	if (!deferPagePresentation)
		presentDisplay();
}

void drawNotificationsScreen()
{
	if (lvglReady)
	{
		renderLvglPage(1);
		return;
	}
	fillRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BACKGROUND);
	drawAppHeader("ALERTS");
	NotificationPreview *alert = selectedAlert();
	if (alert == nullptr)
	{
		fillRoundedRectangle(10, 50, 620, 86, 14, COLOR_PANEL_RAISED);
		fillCircle(54, 93, 22, COLOR_GREEN);
		drawCenteredText(54, 89, "OK", 1, COLOR_BACKGROUND, COLOR_GREEN);
		drawText(92, 68, "ALL CAUGHT UP", 2, COLOR_WHITE, COLOR_PANEL_RAISED);
		drawText(92, 103, "NEW WORDPRESS EVENTS WILL APPEAR HERE", 1, COLOR_MUTED, COLOR_PANEL_RAISED);
	}
	else
	{
		fillRoundedRectangle(10, 50, 620, 86, 14, COLOR_PANEL_RAISED);
		const uint16_t severityColor = alert->severity == "critical" ? COLOR_RED :
			alert->severity == "warning" ? COLOR_AMBER : COLOR_BLUE_LIGHT;
		fillRectangle(10, 62, 5, 62, severityColor);
		drawText(26, 58, alert->source, 1, COLOR_BLUE_LIGHT, COLOR_PANEL_RAISED);
		drawText(548, 58, alert->receivedAt, 1, COLOR_MUTED, COLOR_PANEL_RAISED);
		drawText(26, 78, alert->title, 2, COLOR_WHITE, COLOR_PANEL_RAISED);
		drawText(26, 103, alert->body, 1, COLOR_MUTED, COLOR_PANEL_RAISED);
		drawText(28, 122, String(selectedAlertIndex + 1) + " OF " + String(alertCount), 1,
			alert->unread ? COLOR_BLUE_LIGHT : COLOR_GREEN, COLOR_PANEL_RAISED);
		fillRoundedRectangle(420, 113, 92, 21, 8, selectedAlertIndex > 0 ? COLOR_BLUE : COLOR_PANEL);
		drawCenteredText(466, 119, "NEWER", 1, COLOR_WHITE,
			selectedAlertIndex > 0 ? COLOR_BLUE : COLOR_PANEL);
		fillRoundedRectangle(520, 113, 96, 21, 8,
			selectedAlertIndex + 1 < alertCount ? COLOR_BLUE : COLOR_PANEL);
		drawCenteredText(568, 119, "OLDER", 1, COLOR_WHITE,
			selectedAlertIndex + 1 < alertCount ? COLOR_BLUE : COLOR_PANEL);
	}
	drawBottomNavigation(1);
	if (!deferPagePresentation)
		presentDisplay();
}

void drawDeviceScreen()
{
	if (lvglReady)
	{
		renderLvglPage(2);
		return;
	}
	fillRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BACKGROUND);
	drawAppHeader("DEVICE");
	const String batteryText = batteryAvailable ? String(batteryPercent) + "%" : "USB";
	const String values[] = {displayPairingId(), FIRMWARE_LABEL, batteryText, audioReady ? "READY" : "FAILED"};
	const String labels[] = {"PAIRING ID", "FIRMWARE", "BATTERY", "SOUND"};
	const int widths[] = {190, 130, 130, 150};
	int x = 10;
	for (int index = 0; index < 4; ++index)
	{
		fillRoundedRectangle(x, 50, widths[index], 86, 13, COLOR_PANEL_RAISED);
		drawText(x + 13, 60, labels[index], 2, COLOR_MUTED, COLOR_PANEL_RAISED);
		drawText(x + 13, 91, values[index], values[index].length() > 9 ? 1 : 2,
			index == 2 && batteryAvailable && batteryPercent <= 15 ? COLOR_RED :
			index == 3 && !audioReady ? COLOR_RED : COLOR_WHITE,
			COLOR_PANEL_RAISED);
		x += widths[index] + 10;
	}
	drawBottomNavigation(2);
	if (!deferPagePresentation)
		presentDisplay();
}

void drawIdleClockScreen()
{
	if (lvglReady)
	{
		renderLvglClock();
		return;
	}
	fillRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);

	tm clockTime = {};
	if (readLocalClock(clockTime))
	{
		char timeBuffer[6] = {};
		char dateBuffer[24] = {};
		strftime(timeBuffer, sizeof(timeBuffer), "%H:%M", &clockTime);
		strftime(dateBuffer, sizeof(dateBuffer), "%a %d %b", &clockTime);
		String dateText = String(dateBuffer);
		dateText.toUpperCase();
		drawCenteredText(SCREEN_WIDTH / 2, 42, String(timeBuffer), 10, COLOR_WHITE, COLOR_BLACK);
		drawCenteredText(SCREEN_WIDTH / 2, 137, dateText, 2, COLOR_MUTED, COLOR_BLACK);
		lastClockMinute = clockTime.tm_min;
	}
	else
	{
		drawCenteredText(SCREEN_WIDTH / 2, 42, "--:--", 10, COLOR_WHITE, COLOR_BLACK);
		drawCenteredText(SCREEN_WIDTH / 2, 137, WiFi.isConnected() ? "SYNCING TIME" : "CONNECT WIFI FOR TIME", 2,
			COLOR_MUTED, COLOR_BLACK);
		lastClockMinute = -1;
	}
	if (!deferPagePresentation)
		presentDisplay();
}

void drawCurrentPage()
{
	if (lvglReady)
	{
		if (idleClockActive)
			renderLvglClock();
		else
			renderLvglPage(currentPage);
		return;
	}
	if (idleClockActive)
	{
		drawIdleClockScreen();
		return;
	}
	if (currentPage == 0)
	{
		drawDashboardScreen();
	}
	else if (currentPage == 1)
	{
		drawNotificationsScreen();
	}
	else
	{
		drawDeviceScreen();
	}
}

void handleCompletedTouch()
{
	if (portalRunning)
		return;
	lastInteractionMs = millis();
	const int deltaX = static_cast<int>(touchLastX) - touchStartX;
	const int deltaY = static_cast<int>(touchLastY) - touchStartY;
	const bool horizontalSwipe = abs(deltaX) >= SWIPE_MIN_DISTANCE && abs(deltaX) > abs(deltaY) * 4 / 3;

	if (horizontalSwipe)
	{
		if (deltaX < 0 && currentPage < 2)
		{
			++currentPage;
			drawCurrentPage();
			Serial.printf("[GESTURE] Swiped left to page %u\n", currentPage + 1);
		}
		else if (deltaX > 0 && currentPage > 0)
		{
			--currentPage;
			drawCurrentPage();
			Serial.printf("[GESTURE] Swiped right to page %u\n", currentPage + 1);
		}
		return;
	}

	if (touchLastY >= 138)
	{
		const uint8_t selectedPage = min<uint8_t>(2, touchLastX / (SCREEN_WIDTH / 3));
		if (selectedPage != currentPage)
		{
			currentPage = selectedPage;
			drawCurrentPage();
		}
		return;
	}

	if (currentPage == 1 && selectedAlert() != nullptr)
	{
		if (touchLastY >= 108 && touchLastY < 138 && touchLastX >= 420)
		{
			if (touchLastX < 516 && selectedAlertIndex > 0)
				--selectedAlertIndex;
			else if (touchLastX >= 516 && selectedAlertIndex + 1 < alertCount)
				++selectedAlertIndex;
		}
		else
		{
			selectedAlert()->unread = false;
			Serial.println("[ALERT] Local preview marked read");
		}
		drawNotificationsScreen();
	}
	else if (currentPage == 0 && touchLastX >= 378 && touchLastY >= 96 && !mqttClient.connected())
	{
		startSetupPortal();
	}
}

void injectLocalTestNotification()
{
	addAlert("SITE NEEDS ATTENTION", "NOTIFICATOR TOUCH IS READY", "LOCAL HARDWARE TEST");
	idleClockActive = false;
	lastInteractionMs = millis();
	currentPage = 1;
	drawNotificationsScreen();
	playTestChime();
	Serial.println("[ALERT] Local Notificator test notification created");
}

void initializeButtons()
{
	for (ButtonState &button : buttons)
	{
		pinMode(button.pin, INPUT_PULLUP);
		button.rawPressed = digitalRead(button.pin) == LOW;
		button.pressed = button.rawPressed;
		button.changedAtMs = millis();
		button.pressedAtMs = button.pressed ? millis() : 0;
		button.longActionHandled = false;
	}
}

void updateButtons()
{
	for (ButtonState &button : buttons)
	{
		const bool rawPressed = digitalRead(button.pin) == LOW;
		if (rawPressed != button.rawPressed)
		{
			button.rawPressed = rawPressed;
			button.changedAtMs = millis();
		}
		if (button.pressed != button.rawPressed && millis() - button.changedAtMs >= BUTTON_DEBOUNCE_MS)
		{
			button.pressed = button.rawPressed;
			if (button.pressed)
			{
				noteUserInteraction();
				++button.pressCount;
				button.pressedAtMs = millis();
				button.longActionHandled = false;
				if (button.pin == BUTTON_PIN_POWER)
				{
					buttonPageMessage = "HOLD POWER TO SHUT DOWN";
				}
				// BOOT actions resolve on release so a setup hold cannot also
				// create a test alert.
			}
			else if (button.pin == BUTTON_PIN_POWER && !button.longActionHandled)
			{
				buttonPageMessage = "RESET RESTARTS IMMEDIATELY";
			}
			else if (button.pin == BUTTON_PIN_BOOT && !button.longActionHandled)
			{
				injectLocalTestNotification();
			}
			Serial.printf("[BUTTON] %s %s count=%u\n",
				button.name,
				button.pressed ? "pressed" : "released",
				button.pressCount);
			if (currentPage == 2)
			{
				drawDeviceScreen();
			}
		}

		if (button.pin == BUTTON_PIN_POWER && button.pressed && !button.longActionHandled &&
			millis() - button.pressedAtMs >= POWER_OFF_HOLD_MS)
		{
			button.longActionHandled = true;
			buttonPageMessage = "POWERING OFF  USB MAY STAY ON";
			if (currentPage == 2)
			{
				drawDeviceScreen();
			}
			Serial.println("[BUTTON] PWR long hold: releasing battery power latch");
			delay(120);
			if (!releaseSystemPower())
			{
				buttonPageMessage = "POWER LATCH FAILED";
				if (currentPage == 2)
				{
					drawDeviceScreen();
				}
			}
		}

		if (button.pin == BUTTON_PIN_BOOT && button.pressed && !button.longActionHandled &&
			millis() - button.pressedAtMs >= SETUP_BUTTON_HOLD_MS)
		{
			button.longActionHandled = true;
			Serial.println("[BUTTON] BOOT long hold: opening setup portal");
			startSetupPortal();
		}
	}
}

bool readTouch(uint16_t &screenX, uint16_t &screenY, uint8_t &points)
{
	static const uint8_t readCommand[11] = {
		0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00,
	};
	uint8_t response[32] = {};

	touchWire.beginTransmission(TOUCH_ADDRESS);
	touchWire.write(readCommand, sizeof(readCommand));
	if (touchWire.endTransmission(false) != 0)
	{
		return false;
	}

	const size_t received = touchWire.requestFrom(TOUCH_ADDRESS, static_cast<uint8_t>(sizeof(response)));
	if (received != sizeof(response))
	{
		return false;
	}
	for (size_t index = 0; index < sizeof(response); ++index)
	{
		response[index] = touchWire.read();
	}

	points = response[1];
	if (points == 0 || points >= 5)
	{
		return false;
	}

	const uint16_t rawX = ((static_cast<uint16_t>(response[2]) & 0x0F) << 8) | response[3];
	const uint16_t rawY = ((static_cast<uint16_t>(response[4]) & 0x0F) << 8) | response[5];
	// In the physical landscape orientation the controller's long raw axis is X
	// and its short raw axis is Y.
	const uint16_t physicalX = constrain(rawX, 0, SCREEN_WIDTH - 1);
	const uint16_t physicalY = constrain(rawY, 0, SCREEN_HEIGHT - 1);
	screenX = displayFlipped ? SCREEN_WIDTH - 1 - physicalX : physicalX;
	screenY = displayFlipped ? SCREEN_HEIGHT - 1 - physicalY : physicalY;
	return true;
}

void printRuntimeReport()
{
	esp_chip_info_t chipInfo = {};
	esp_chip_info(&chipInfo);
	Serial.println();
	Serial.println("=== Notificator Touch runtime ===");
	Serial.printf("Model: %s\n", MODEL_ID);
	Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);
	Serial.printf("Chip: ESP32-S3 revision %d.%d, %d cores\n", chipInfo.revision / 100, chipInfo.revision % 100, chipInfo.cores);
	Serial.printf("Flash: %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
	Serial.printf("PSRAM: %u MB, integrity: %s\n", ESP.getPsramSize() / (1024 * 1024), psramReady ? "PASS" : "FAIL");
	Serial.printf("System I2C: RTC=%s IMU=%s\n", rtcFound ? "PASS" : "MISSING", imuFound ? "PASS" : "MISSING");
	Serial.printf("Touch I2C: controller=%s\n", touchControllerFound ? "PASS" : "MISSING");
	Serial.printf("Display: %s\n", displayReady ? "PASS" : "FAIL");
	Serial.printf("Audio: %s\n", audioReady ? "PASS" : "FAIL");
	if (batteryAvailable)
		Serial.printf("Battery: %u%% (%.2f V estimated)\n", batteryPercent, batteryVoltage);
	else
		Serial.println("Battery: not detected (USB power)");
	Serial.printf("IMU auto-rotation: %s\n", imuRotationReady ? "PASS" : "FAIL");
	Serial.println("Buttons: BOOT=GPIO0 POWER=GPIO16");
	Serial.printf("Wi-Fi: %s\n", WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "not connected");
	Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
	Serial.printf("Free PSRAM: %u bytes\n", ESP.getFreePsram());
	Serial.println("Short BOOT creates a test alert; hold BOOT for setup.");
	Serial.println("================================");
}
} // namespace

void setup()
{
	Serial.begin(115200);
	delay(800);
	Serial.println("[BOOT] Starting Notificator Touch");
	deviceId = String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
	deviceId.toLowerCase();
	setupSsid = String(WIFI_AP_PREFIX) + pairingId();
	setupSsid.toUpperCase();

	pulseBacklight();
	psramReady = testPsram();
	initializeButtons();
	initializeBatteryMonitor();
	lastInteractionMs = millis();

	Wire.begin(SYSTEM_PIN_SDA, SYSTEM_PIN_SCL, 300000);
	touchWire.begin(TOUCH_PIN_SDA, TOUCH_PIN_SCL, 300000);
	if (!holdSystemPower())
	{
		Serial.println("[POWER] Battery power latch was not available");
	}
	scanBus(Wire, "system");
	scanBus(touchWire, "touch");
	rtcFound = probeAddress(Wire, RTC_ADDRESS);
	imuFound = probeAddress(Wire, IMU_ADDRESS);
	touchControllerFound = probeAddress(touchWire, TOUCH_ADDRESS);
	imuRotationReady = initializeImuRotation();

	if (!initializeDisplay())
	{
		Serial.println("[FAIL] Display initialization failed");
		return;
	}
	initializeLvglUi();
	drawBootSplash();
	audioReady = initializeAudio();
	delay(900);

	loadConfiguration();
	applyScreenBrightness();
	applySoundVolume();
	configureSetupPortal();
	configureMqttClient();
	WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t)
		{
			if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
			{
				clockSyncStarted = false;
				networkStateChanged = true;
			}
			else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
				networkStateChanged = true;
		});

	if (deviceConfigured && mqttConfigValid)
	{
		WiFi.mode(WIFI_STA);
		WiFi.begin();
		drawCurrentPage();
	}
	else
	{
		startSetupPortal();
	}
	printRuntimeReport();
}

void loop()
{
	if (!displayReady || !touchControllerFound)
	{
		delay(250);
		return;
	}
	if (lvglReady)
		lv_timer_handler();
	updateOrientation();
	updateButtons();

	if (wifiWizardActive && wifiWizardStage == WifiWizardStage::Connecting)
	{
		if (WiFi.isConnected() && WiFi.SSID() == pendingWifiSsid)
		{
			wifiWizardStage = WifiWizardStage::Success;
			clockSyncStarted = false;
			lastMqttAttemptMs = 0;
			networkStateChanged = false;
			Serial.printf("[WIFI] Connection accepted: %s\n", pendingWifiSsid.c_str());
			renderWifiResult(true);
		}
		else if (millis() - wifiConnectStartedMs >= WIFI_WIZARD_TIMEOUT_MS)
		{
			wifiWizardStage = WifiWizardStage::Failed;
			restorePreviousWifi();
			networkStateChanged = false;
			Serial.printf("[WIFI] Connection test failed: %s\n", pendingWifiSsid.c_str());
			renderWifiResult(false);
		}
	}

	if (portalRunning)
	{
		wifiManager.process();
		if (WiFi.isConnected() && portalSaveRequested)
		{
			if (savePortalConfiguration())
			{
				// WiFiManager already shuts its server down after a successful
				// non-blocking connection. Calling stopConfigPortal() again can
				// dereference the released server on WiFiManager 2.0.17.
				portalRunning = false;
				configureMqttClient();
				drawSetupScreen("SAVED  CONNECTING SECURELY");
				delay(700);
				currentPage = 0;
				drawCurrentPage();
			}
			else
			{
				drawSetupScreen("CHECK THE HIVEMQ FIELDS");
				portalSaveRequested = false;
			}
		}
		delay(10);
		return;
	}

	if (WiFi.isConnected() && mqttConfigValid)
	{
		startClockSync();
		if (!mqttClient.connected() && millis() - lastMqttAttemptMs >= MQTT_RECONNECT_MS)
		{
			lastMqttAttemptMs = millis();
			connectMqtt();
		}
		else if (mqttClient.connected())
		{
			const bool wasHealthy = millis() - lastMqttHealthMs <= MQTT_HEALTH_GRACE_MS;
			if (mqttClient.loop())
				lastMqttHealthMs = millis();
			const bool isHealthy = mqttClient.connected() && millis() - lastMqttHealthMs <= MQTT_HEALTH_GRACE_MS;
			if (wasHealthy != isHealthy)
				networkStateChanged = true;
			if (millis() - lastMqttStatusMs >= MQTT_STATUS_INTERVAL_MS)
				publishDeviceStatus("heartbeat");
		}
	}
	if (!wifiWizardActive && !portalRunning)
		maybeRefreshWeather();

	if (millis() - lastBatterySampleMs >= BATTERY_SAMPLE_INTERVAL_MS)
	{
		const bool batteryDisplayChanged = sampleBatteryLevel();
		if (batteryDisplayChanged && !wifiWizardActive)
			drawCurrentPage();
	}

	if (networkStateChanged)
	{
		networkStateChanged = false;
		if (!wifiWizardActive)
			drawCurrentPage();
	}

	// A gentle green pulse communicates an actively serviced MQTT keepalive.
	// Stop repainting in clock mode so the idle display remains still and dark.
	if (!lvglReady && !idleClockActive && mqttClient.connected() &&
		millis() - lastMqttPulseRenderMs >= MQTT_PULSE_INTERVAL_MS)
	{
		lastMqttPulseRenderMs = millis();
		drawCurrentPage();
	}

	if (!lvglReady)
	{
		uint16_t x = 0;
		uint16_t y = 0;
		uint8_t points = 0;
		if (readTouch(x, y, points))
		{
			missedTouchReads = 0;
			if (idleClockActive)
			{
				noteUserInteraction();
				touchActive = false;
				delay(80);
				return;
			}
			if (!touchActive)
			{
				touchActive = true;
				touchStartX = x;
				touchStartY = y;
			}
			touchLastX = x;
			touchLastY = y;
			if (millis() - lastTouchLogMs >= 80)
			{
				Serial.printf("[TOUCH] points=%u x=%u y=%u\n", points, x, y);
				lastTouchLogMs = millis();
			}
		}
		else if (touchActive && ++missedTouchReads >= 2)
		{
			touchActive = false;
			missedTouchReads = 0;
			handleCompletedTouch();
		}
	}

	if (!wifiWizardActive && !idleClockActive && !touchActive &&
		millis() - lastInteractionMs >= IDLE_CLOCK_TIMEOUT_MS)
	{
		idleClockActive = true;
		lastClockSecond = -1;
		drawIdleClockScreen();
	}
	else if (idleClockActive)
	{
		tm clockTime = {};
		const int currentSecond = readLocalClock(clockTime)
			? clockTime.tm_sec
			: static_cast<int>((millis() / 1000) % 60);
		if (currentSecond != lastClockSecond)
			drawIdleClockScreen();
	}

	delay(16);
}
