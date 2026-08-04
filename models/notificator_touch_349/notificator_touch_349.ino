#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Wire.h>
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

#include "src/axs15231b/esp_lcd_axs15231b.h"
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "portal_ui.h"

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
constexpr char FIRMWARE_VERSION[] = "1.2.0";
constexpr char FIRMWARE_LABEL[] = "1.2.0 DEV";
constexpr char MODEL_ID[] = "notificator_touch_349";
constexpr char WIFI_AP_PREFIX[] = "WPNOTIF-";
constexpr char DEFAULT_MQTT_TOPIC_PREFIX[] = "notificator-project";
constexpr uint16_t DEFAULT_MQTT_PORT = 8883;
constexpr unsigned long MQTT_RECONNECT_MS = 5000;
constexpr unsigned long MQTT_STATUS_INTERVAL_MS = 60000;
constexpr unsigned long MQTT_HEALTH_GRACE_MS = 5000;
constexpr unsigned long MQTT_PULSE_INTERVAL_MS = 1000;
constexpr unsigned long BATTERY_SAMPLE_INTERVAL_MS = 30000;
constexpr unsigned long IDLE_CLOCK_TIMEOUT_MS = 60000;
constexpr unsigned long SETUP_BUTTON_HOLD_MS = 4000;

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
bool clockSyncStarted = false;
bool batteryAvailable = false;
unsigned long lastMqttAttemptMs = 0;
unsigned long lastMqttStatusMs = 0;
unsigned long lastMqttHealthMs = 0;
unsigned long lastMqttPulseRenderMs = 0;
unsigned long lastBatterySampleMs = 0;
unsigned long lastInteractionMs = 0;
int lastClockMinute = -1;
float batteryVoltage = 0.0F;
uint8_t batteryPercent = 0;
int16_t clockUtcOffsetMinutes = 0;

Preferences preferences;
WiFiManager wifiManager;
WiFiClientSecure mqttTlsClient;
PubSubClient mqttClient(mqttTlsClient);

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
	bool available;
	bool unread;
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

void addAlert(String title, String body, String source)
{
	title.toUpperCase();
	body.toUpperCase();
	source.toUpperCase();
	title = title.substring(0, 30);
	body = body.substring(0, 52);
	source = source.substring(0, 30);

	const uint8_t lastIndex = min<uint8_t>(alertCount, MAX_ALERT_HISTORY - 1);
	for (int index = lastIndex; index > 0; --index)
		alertHistory[index] = alertHistory[index - 1];
	alertHistory[0] = {title, body, source, true, true};
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

void animateOrientationChange(bool desiredFlip)
{
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
	configTime(static_cast<long>(clockUtcOffsetMinutes) * 60L, 0, "pool.ntp.org", "time.nist.gov");
	clockSyncStarted = true;
	Serial.printf("[CLOCK] NTP requested with UTC offset %+d minutes\n", clockUtcOffsetMinutes);
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

void handleIncomingMqtt(char *topic, uint8_t *payload, unsigned int length)
{
	const String receivedTopic = String(topic ? topic : "");
	if (receivedTopic != mqttMessageTopic && receivedTopic != mqttLegacyMessageTopic)
		return;
	String raw;
	raw.reserve(length + 1);
	for (unsigned int index = 0; index < length; ++index)
		raw += static_cast<char>(payload[index]);

	JsonDocument document;
	const DeserializationError error = deserializeJson(document, raw);
	if (!error && document.is<JsonObject>())
	{
		addAlert(
			String(document["title"] | "NEW NOTIFICATION"),
			String(document["body"] | document["message"] | "OPEN THE APP FOR DETAILS"),
			String(document["site"] | document["source"] | "NOTIFICATOR"));
	}
	else
	{
		addAlert("NEW NOTIFICATION", raw, "NOTIFICATOR");
	}
	idleClockActive = false;
	lastInteractionMs = millis();
	currentPage = 1;
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

bool publishDeviceStatus(const char *eventName = "online")
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
		fillRectangle(10, 62, 5, 62, alert->unread ? COLOR_BLUE_LIGHT : COLOR_GREEN);
		drawText(26, 58, alert->source, 1, COLOR_BLUE_LIGHT, COLOR_PANEL_RAISED);
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
	fillRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);

	if (batteryAvailable)
		drawText(574, 13, String(batteryPercent) + "%", 2,
			batteryPercent <= 15 ? COLOR_RED : COLOR_MUTED, COLOR_BLACK);

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
	drawBootSplash();
	audioReady = initializeAudio();
	delay(900);

	loadConfiguration();
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
	updateOrientation();
	updateButtons();

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

	if (millis() - lastBatterySampleMs >= BATTERY_SAMPLE_INTERVAL_MS)
	{
		const bool batteryDisplayChanged = sampleBatteryLevel();
		if (batteryDisplayChanged)
			drawCurrentPage();
	}

	if (networkStateChanged)
	{
		networkStateChanged = false;
		drawCurrentPage();
	}

	// A gentle green pulse communicates an actively serviced MQTT keepalive.
	// Stop repainting in clock mode so the idle display remains still and dark.
	if (!idleClockActive && mqttClient.connected() &&
		millis() - lastMqttPulseRenderMs >= MQTT_PULSE_INTERVAL_MS)
	{
		lastMqttPulseRenderMs = millis();
		drawCurrentPage();
	}

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

	if (!idleClockActive && !touchActive && millis() - lastInteractionMs >= IDLE_CLOCK_TIMEOUT_MS)
	{
		idleClockActive = true;
		drawIdleClockScreen();
	}
	else if (idleClockActive)
	{
		tm clockTime = {};
		if (readLocalClock(clockTime) && clockTime.tm_min != lastClockMinute)
			drawIdleClockScreen();
	}

	delay(16);
}
