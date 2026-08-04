#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <math.h>

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

/**
 * @file notificator_touch_349_diagnostic.ino
 * @brief Non-production hardware diagnostic for the Waveshare 3.49-inch board.
 *
 * This sketch deliberately contains no Notificator credentials, MQTT client,
 * setup portal, persistence, or OTA behavior. Its only job is to establish a
 * known-good display, touch, memory, I2C, and Wi-Fi baseline on real hardware.
 */

namespace
{
constexpr char DIAGNOSTIC_VERSION[] = "0.6.0";

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

constexpr int SYSTEM_PIN_SDA = 47;
constexpr int SYSTEM_PIN_SCL = 48;
constexpr uint8_t RTC_ADDRESS = 0x51;
constexpr uint8_t IMU_ADDRESS = 0x6B;
constexpr uint8_t AUDIO_CODEC_ADDRESS = 0x18;
constexpr uint8_t IO_EXPANDER_ADDRESS = 0x20;

constexpr int SOUND_BUTTON_LEFT = 406;
constexpr int SOUND_BUTTON_TOP = 62;
constexpr int SOUND_BUTTON_WIDTH = 198;
constexpr int SOUND_BUTTON_HEIGHT = 66;
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

constexpr int TOUCH_ZONE_LEFT = 216;
constexpr int TOUCH_ZONE_TOP = 94;
constexpr int TOUCH_ZONE_WIDTH = 412;
constexpr int TOUCH_ZONE_HEIGHT = 66;

constexpr uint16_t COLOR_BACKGROUND = 0x0611;
constexpr uint16_t COLOR_PANEL = 0x1224;
constexpr uint16_t COLOR_BLUE = 0x2B5D;
constexpr uint16_t COLOR_BLUE_LIGHT = 0x4C1F;
constexpr uint16_t COLOR_GREEN = 0x05EC;
constexpr uint16_t COLOR_CYAN = 0x27FF;
constexpr uint16_t COLOR_RED = 0xF986;
constexpr uint16_t COLOR_MUTED = 0x9D18;
constexpr uint16_t COLOR_WHITE = 0xFFFF;

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
String interactionStatus = "READY";
int8_t orientationAxis = 1;
int8_t orientationReferenceSign = 1;
bool orientationCandidate = false;
bool deferPagePresentation = false;

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
	uint16_t pixels[GLYPH_WIDTH * 3 * GLYPH_HEIGHT * 3];
	const int bitmapWidth = GLYPH_WIDTH * scale;
	const int bitmapHeight = GLYPH_HEIGHT * scale;
	const uint8_t *glyph = glyphFor(character);
	const uint16_t foregroundColor = foreground;
	const uint16_t backgroundColor = background;

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
			pixels[row * bitmapWidth + column] = enabled ? foregroundColor : backgroundColor;
		}
	}

	for (int row = 0; row < bitmapHeight; ++row)
	{
		for (int column = 0; column < bitmapWidth; ++column)
		{
			const int targetX = x + column;
			const int targetY = y + row;
			if (targetX >= 0 && targetX < SCREEN_WIDTH && targetY >= 0 && targetY < SCREEN_HEIGHT)
			{
				frameBuffer[static_cast<size_t>(targetY) * SCREEN_WIDTH + targetX] =
					pixels[row * bitmapWidth + column];
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
			interactionStatus = "ROTATION READY";
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
	interactionStatus = desiredFlip ? "FLIPPED" : "ROTATION PASSED";
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

void drawStatusCard(int x, int y, int width, const String &label, const String &value, bool success)
{
	fillRectangle(x, y, width, 70, COLOR_PANEL);
	drawText(x + 10, y + 10, label, 1, COLOR_MUTED, COLOR_PANEL);
	drawText(x + 10, y + 36, value, 2, success ? COLOR_GREEN : COLOR_RED, COLOR_PANEL);
}

void markTouch(uint16_t screenX, uint16_t screenY);

void drawDiagnosticScreen()
{
	fillRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BACKGROUND);
	fillRectangle(0, 0, 204, SCREEN_HEIGHT, COLOR_BLUE);
	drawText(18, 22, "NOTIFICATOR", 2, COLOR_WHITE, COLOR_BLUE);
	drawText(18, 60, "TOUCH", 2, COLOR_WHITE, COLOR_BLUE);
	drawText(18, 96, "HARDWARE DIAGNOSTIC", 1, COLOR_WHITE, COLOR_BLUE);
	drawText(18, 126, "SWIPE LEFT", 1, COLOR_WHITE, COLOR_BLUE);

	drawStatusCard(216, 12, 94, "DISPLAY", "OK", displayReady);
	drawStatusCard(318, 12, 94, "PSRAM", psramReady ? "8 MB" : "FAIL", psramReady);
	drawStatusCard(420, 12, 94, "FLASH", String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB", ESP.getFlashChipSize() == 16 * 1024 * 1024);
	drawStatusCard(522, 12, 106, "WIFI", wifiNetworkCount >= 0 ? String(wifiNetworkCount) : "FAIL", wifiNetworkCount >= 0);

	fillRectangle(TOUCH_ZONE_LEFT, TOUCH_ZONE_TOP, TOUCH_ZONE_WIDTH, TOUCH_ZONE_HEIGHT, COLOR_PANEL);
	drawOutline(TOUCH_ZONE_LEFT, TOUCH_ZONE_TOP, TOUCH_ZONE_WIDTH, TOUCH_ZONE_HEIGHT, COLOR_BLUE_LIGHT, 2);
	drawText(232, 106, "TOUCH OR SWIPE LEFT", 1, COLOR_MUTED, COLOR_PANEL);
	if (!deferPagePresentation)
	{
		presentDisplay();
	}
}

void drawInteractionScreen()
{
	fillRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BACKGROUND);
	fillRectangle(0, 0, 204, SCREEN_HEIGHT, COLOR_BLUE);
	drawText(18, 22, "GESTURE", 2, COLOR_WHITE, COLOR_BLUE);
	drawText(18, 60, "AND AUDIO", 2, COLOR_WHITE, COLOR_BLUE);
	drawText(18, 100, "PAGE TWO", 1, COLOR_WHITE, COLOR_BLUE);
	drawText(18, 126, "SWIPE RIGHT", 1, COLOR_WHITE, COLOR_BLUE);

	drawText(224, 18, "HORIZONTAL SWIPE TEST", 1, COLOR_MUTED, COLOR_BACKGROUND);
	drawText(224, 40, interactionStatus, 2,
		interactionStatus == "FAILED" ? COLOR_RED : COLOR_GREEN,
		COLOR_BACKGROUND);

	fillRectangle(SOUND_BUTTON_LEFT, SOUND_BUTTON_TOP, SOUND_BUTTON_WIDTH, SOUND_BUTTON_HEIGHT,
		audioReady ? COLOR_BLUE : COLOR_PANEL);
	drawOutline(SOUND_BUTTON_LEFT, SOUND_BUTTON_TOP, SOUND_BUTTON_WIDTH, SOUND_BUTTON_HEIGHT,
		audioReady ? COLOR_BLUE_LIGHT : COLOR_RED, 2);
	drawText(SOUND_BUTTON_LEFT + 24, SOUND_BUTTON_TOP + 13, "PLAY", 2,
		audioReady ? COLOR_WHITE : COLOR_MUTED,
		audioReady ? COLOR_BLUE : COLOR_PANEL);
	drawText(SOUND_BUTTON_LEFT + 24, SOUND_BUTTON_TOP + 39,
		audioReady ? "TEST CHIME" : "AUDIO UNAVAILABLE", 1,
		audioReady ? COLOR_WHITE : COLOR_RED,
		audioReady ? COLOR_BLUE : COLOR_PANEL);

	drawText(224, 142,
		orientationCalibrated ? "SWIPE RIGHT  AUTO ROTATE ON" : "SWIPE RIGHT  HOLD UPRIGHT",
		1,
		orientationCalibrated ? COLOR_MUTED : COLOR_RED,
		COLOR_BACKGROUND);
	if (!deferPagePresentation)
	{
		presentDisplay();
	}
}

void drawButtonCard(int x, const ButtonState &button)
{
	constexpr int cardTop = 42;
	constexpr int cardWidth = 188;
	constexpr int cardHeight = 86;
	const uint16_t cardColor = button.pressed ? COLOR_BLUE : COLOR_PANEL;
	const uint16_t stateColor = button.pressed ? COLOR_WHITE : COLOR_GREEN;
	fillRectangle(x, cardTop, cardWidth, cardHeight, cardColor);
	drawOutline(x, cardTop, cardWidth, cardHeight,
		button.pressed ? COLOR_CYAN : COLOR_BLUE_LIGHT, 2);
	drawText(x + 12, cardTop + 10, button.name, 1, COLOR_MUTED, cardColor);
	drawText(x + 12, cardTop + 34, button.pressed ? "PRESSED" : "RELEASED", 2, stateColor, cardColor);
	drawText(x + 12, cardTop + 68, "COUNT " + String(button.pressCount), 1, COLOR_MUTED, cardColor);
}

void drawButtonTestScreen()
{
	fillRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BACKGROUND);
	fillRectangle(0, 0, 204, SCREEN_HEIGHT, COLOR_BLUE);
	drawText(18, 22, "PHYSICAL", 2, COLOR_WHITE, COLOR_BLUE);
	drawText(18, 60, "BUTTONS", 2, COLOR_WHITE, COLOR_BLUE);
	drawText(18, 100, "PAGE THREE", 1, COLOR_WHITE, COLOR_BLUE);
	drawText(18, 126, "SWIPE RIGHT", 1, COLOR_WHITE, COLOR_BLUE);

	drawText(224, 16, "PRESS EACH SIDE BUTTON", 1, COLOR_MUTED, COLOR_BACKGROUND);
	drawButtonCard(224, buttons[0]);
	drawButtonCard(420, buttons[1]);
	drawText(224, 145, buttonPageMessage, 1,
		buttonPageMessage == "RESET RESTARTS IMMEDIATELY" ? COLOR_RED : COLOR_CYAN,
		COLOR_BACKGROUND);
	if (!deferPagePresentation)
	{
		presentDisplay();
	}
}

void drawCurrentPage()
{
	if (currentPage == 0)
	{
		drawDiagnosticScreen();
	}
	else if (currentPage == 1)
	{
		drawInteractionScreen();
	}
	else
	{
		drawButtonTestScreen();
	}
}

bool pointInside(int x, int y, int left, int top, int width, int height)
{
	return x >= left && x < left + width && y >= top && y < top + height;
}

void handleCompletedTouch()
{
	const int deltaX = static_cast<int>(touchLastX) - touchStartX;
	const int deltaY = static_cast<int>(touchLastY) - touchStartY;
	const bool horizontalSwipe = abs(deltaX) >= SWIPE_MIN_DISTANCE && abs(deltaX) > abs(deltaY) * 4 / 3;

	if (horizontalSwipe)
	{
		if (deltaX < 0 && currentPage < 2)
		{
			++currentPage;
			if (currentPage == 1)
			{
				interactionStatus = "SWIPE PASSED";
			}
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

	if (currentPage == 1 && pointInside(
		touchLastX,
		touchLastY,
		SOUND_BUTTON_LEFT,
		SOUND_BUTTON_TOP,
		SOUND_BUTTON_WIDTH,
		SOUND_BUTTON_HEIGHT))
	{
		interactionStatus = "PLAYING";
		drawInteractionScreen();
		interactionStatus = playTestChime() ? "CHIME PASSED" : "FAILED";
		drawInteractionScreen();
	}
	else if (currentPage == 0)
	{
		markTouch(touchLastX, touchLastY);
	}
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
				++button.pressCount;
				button.pressedAtMs = millis();
				button.longActionHandled = false;
				if (button.pin == BUTTON_PIN_POWER)
				{
					buttonPageMessage = "HOLD POWER TO SHUT DOWN";
				}
			}
			else if (button.pin == BUTTON_PIN_POWER && !button.longActionHandled)
			{
				buttonPageMessage = "RESET RESTARTS IMMEDIATELY";
			}
			Serial.printf("[BUTTON] %s %s count=%u\n",
				button.name,
				button.pressed ? "pressed" : "released",
				button.pressCount);
			if (currentPage == 2)
			{
				drawButtonTestScreen();
			}
		}

		if (button.pin == BUTTON_PIN_POWER && button.pressed && !button.longActionHandled &&
			millis() - button.pressedAtMs >= POWER_OFF_HOLD_MS)
		{
			button.longActionHandled = true;
			buttonPageMessage = "POWERING OFF  USB MAY STAY ON";
			if (currentPage == 2)
			{
				drawButtonTestScreen();
			}
			Serial.println("[BUTTON] PWR long hold: releasing battery power latch");
			delay(120);
			if (!releaseSystemPower())
			{
				buttonPageMessage = "POWER LATCH FAILED";
				if (currentPage == 2)
				{
					drawButtonTestScreen();
				}
			}
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

void markTouch(uint16_t screenX, uint16_t screenY)
{
	// Draw in physical screen coordinates. The previous diagnostic scaled the
	// whole panel into the test box, which made a correct touch look offset.
	const int markerX = constrain(static_cast<int>(screenX) - 3, 0, SCREEN_WIDTH - 7);
	const int markerY = constrain(static_cast<int>(screenY) - 3, 0, SCREEN_HEIGHT - 7);
	fillRectangle(markerX, markerY, 7, 7, COLOR_CYAN);
	presentDisplay();
}

void printDiagnosticReport()
{
	esp_chip_info_t chipInfo = {};
	esp_chip_info(&chipInfo);
	Serial.println();
	Serial.println("=== Notificator Touch hardware diagnostic ===");
	Serial.printf("Diagnostic version: %s\n", DIAGNOSTIC_VERSION);
	Serial.printf("Chip: ESP32-S3 revision %d.%d, %d cores\n", chipInfo.revision / 100, chipInfo.revision % 100, chipInfo.cores);
	Serial.printf("Flash: %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
	Serial.printf("PSRAM: %u MB, integrity: %s\n", ESP.getPsramSize() / (1024 * 1024), psramReady ? "PASS" : "FAIL");
	Serial.printf("System I2C: RTC=%s IMU=%s\n", rtcFound ? "PASS" : "MISSING", imuFound ? "PASS" : "MISSING");
	Serial.printf("Touch I2C: controller=%s\n", touchControllerFound ? "PASS" : "MISSING");
	Serial.printf("Display: %s\n", displayReady ? "PASS" : "FAIL");
	Serial.printf("Audio: %s\n", audioReady ? "PASS" : "FAIL");
	Serial.printf("IMU auto-rotation: %s\n", imuRotationReady ? "PASS" : "FAIL");
	Serial.println("Buttons: BOOT=GPIO0 POWER=GPIO16");
	Serial.printf("Wi-Fi scan: %d networks\n", wifiNetworkCount);
	Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
	Serial.printf("Free PSRAM: %u bytes\n", ESP.getFreePsram());
	Serial.println("Touch the screen to verify coordinate mapping.");
	Serial.println("=============================================");
}
} // namespace

void setup()
{
	Serial.begin(115200);
	delay(800);
	Serial.println("[BOOT] Starting Notificator Touch diagnostic");

	pulseBacklight();
	psramReady = testPsram();
	initializeButtons();

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
	audioReady = initializeAudio();

	WiFi.mode(WIFI_STA);
	WiFi.disconnect(false, false);
	wifiNetworkCount = WiFi.scanNetworks(false, true);
	WiFi.scanDelete();

	drawCurrentPage();
	printDiagnosticReport();
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

	uint16_t x = 0;
	uint16_t y = 0;
	uint8_t points = 0;
	if (readTouch(x, y, points))
	{
		missedTouchReads = 0;
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

	delay(16);
}
