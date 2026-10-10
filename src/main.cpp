#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <TinyGPS++.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>

#include "config.h"
#include "speed_state.h"
#include "rmc_reader.h"

#ifdef DISPLAY_DIAGNOSTIC
namespace {
TFT_eSPI diagnostic_tft;
const uint16_t test_colors[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
const char *test_names[] = {"RED", "GREEN", "BLUE", "WHITE"};
uint32_t last_test_ms;
unsigned int test_index;

void draw_test()
{
	diagnostic_tft.fillScreen(test_colors[test_index]);
	diagnostic_tft.setTextColor(TFT_BLACK, test_colors[test_index]);
	diagnostic_tft.drawString("LCD TEST", 20, 30, 4);
	diagnostic_tft.drawString(test_names[test_index], 20, 80, 4);
	Serial.printf("LCD TEST: %s, backlight GPIO%d HIGH\n",
		      test_names[test_index], TFT_BL);
}
}

void setup()
{
	Serial.begin(115200);
	pinMode(TFT_BL, OUTPUT);
	digitalWrite(TFT_BL, HIGH);
	diagnostic_tft.init();
	diagnostic_tft.setRotation(Config::rotation);
	digitalWrite(TFT_BL, HIGH);
	Serial.println("Display-only diagnostic: touch/GPS/PWM/sprites disabled");
	draw_test();
	last_test_ms = millis();
}

void loop()
{
	if (millis() - last_test_ms >= 2000) {
		last_test_ms = millis();
		test_index = (test_index + 1) % 4;
		draw_test();
	}
	delay(1);
}
#else
#include "track_logger.h"
#include "boot_splash.h"
#include "racing_digits.h"
namespace {
TrackLogger track;
HardwareSerial gps_uart(2);
TinyGPSPlus gps;
TFT_eSPI tft;
TFT_eSprite frame(&tft);
SPIClass touch_spi(VSPI);
XPT2046_Touchscreen touch(33, 36);
SpeedState speed(Config::stale_ms, Config::stop_threshold_kmh,
		 Config::maximum_kmh);
RmcReader rmc(gps, speed);
uint32_t last_byte_ms;
uint32_t byte_count;
uint32_t last_render_ms;
uint32_t last_diagnostics_ms;
uint32_t press_ms;
bool pressed;
bool held;
bool night;
bool sprite_ready;
bool splash_visible;
uint32_t splash_start_ms;

void poll_gps()
{
	// Bound each pass so excessive UART traffic cannot starve UI/watchdog.
	for (int n = 0; n < 512 && gps_uart.available(); ++n) {
		const char c = static_cast<char>(gps_uart.read());
		last_byte_ms = millis();
		++byte_count;
		rmc.feed(c, last_byte_ms);
		track.feed(c, last_byte_ms);
	}
	speed.expire(millis());
}

void set_brightness()
{
	ledcWrite(Config::backlight_channel,
		  night ? Config::night_brightness : Config::day_brightness);
}

void poll_controls(uint32_t now)
{
	// Entire screen is one control: no board-specific coordinate calibration.
	const bool down = digitalRead(0) == LOW ||
		(Config::resistive_touch_enabled && touch.touched());
	if (down && !pressed) {
		press_ms = now;
		held = false;
		pressed = true;
	}
	if (down && pressed && !held && now - press_ms >= 2000) {
		held = true;
		Serial.println(speed.reset_if_stopped(now) ?
			       "Session trip reset" : "Reset requires fresh zero speed");
	}
	if (!down && pressed) {
		if (!held && now - press_ms >= 60) {
			night = !night;
			set_brightness();
		}
		pressed = false;
	}
}

enum UiColor : uint8_t {
	UI_BLACK, UI_GREY, UI_CYAN, UI_ORANGE, UI_RED, UI_DARK_GREY, UI_GREEN, UI_WHITE
};
const uint16_t ui_palette[] = {
	TFT_BLACK, TFT_LIGHTGREY, TFT_CYAN, TFT_ORANGE, TFT_RED,
	TFT_DARKGREY, TFT_GREEN, TFT_WHITE
};

template<typename Display>
void draw_ui(Display &canvas, uint32_t now, bool indexed)
{
	const auto color = [indexed](UiColor index) -> uint16_t {
		return indexed ? static_cast<uint16_t>(index) : ui_palette[index];
	};
	const int width = canvas.width();
	const int height = canvas.height();
	const int center = width / 2;
	const bool fresh = speed.fresh(now);
	const bool warning = fresh && speed.speed() >= Config::speed_warning_kmh;
	const bool red_phase = warning && (now / Config::speed_warning_blink_ms) % 2 == 0;
	const uint16_t background = color(red_phase ? UI_RED : UI_BLACK);
	canvas.fillRect(0, 0, width, height, background);
	canvas.setTextSize(1);
	canvas.setTextDatum(TL_DATUM);
	canvas.setTextColor(color(UI_GREY), background);
	canvas.drawString("PALACITA 135", 16, 16, 2);
	canvas.setTextDatum(TR_DATUM);
	canvas.drawString(night ? "NIGHT" : "DAY", width - 16, 16, 2);
	if (warning) {
		canvas.setTextDatum(TC_DATUM);
		canvas.setTextColor(color(UI_WHITE), background);
		canvas.drawString("TOO FAST", center, 16, 2);
	}
	canvas.setTextDatum(MC_DATUM);
	canvas.setTextColor(color(fresh ? UI_WHITE : UI_ORANGE), background);
	char text[64];
	canvas.setTextSize(2);
	if (fresh) {
		snprintf(text, sizeof(text), "%.0f", speed.speed());
		RacingDigits::draw(canvas, text, center, height / 2 - 20, color(UI_WHITE));
	} else {
		RacingDigits::draw(canvas, "0", center, height / 2 - 20, color(UI_WHITE));
	}
	canvas.setTextSize(1);
	canvas.setTextColor(color(UI_GREY), background);
	canvas.drawString("km/h", center, height - 85, 4);
	canvas.drawFastHLine(16, height - 65, width - 32, color(red_phase ? UI_WHITE : UI_DARK_GREY));
	canvas.setTextDatum(TL_DATUM);
	snprintf(text, sizeof(text), "TRIP %.2f km", speed.trip());
	canvas.drawString(text, 16, height - 53, 2);
	canvas.setTextDatum(TR_DATUM);
	snprintf(text, sizeof(text), "MAX %.0f", speed.maximum());
	canvas.drawString(text, width - 16, height - 53, 2);
	canvas.setTextDatum(TL_DATUM);
	const char *state = fresh ? "GPS FIX" : "WAIT FIX / RMC";
	if (!byte_count || now - last_byte_ms > Config::stale_ms)
		state = "NO GPS DATA";
	canvas.setTextColor(color(red_phase ? UI_WHITE : (fresh ? UI_GREEN : UI_ORANGE)), background);
	if (gps.satellites.isValid() && gps.satellites.age() <= Config::stale_ms)
		snprintf(text, sizeof(text), "%s  SAT %lu", state,
			 static_cast<unsigned long>(gps.satellites.value()));
	else
		snprintf(text, sizeof(text), "%s  SAT --", state);
	canvas.drawString(text, 16, height - 27, 2);
	canvas.setTextDatum(TR_DATUM);
	canvas.setTextColor(color(red_phase ? UI_WHITE : UI_DARK_GREY), background);
	canvas.drawString(track.state(), width - 16, height - 23, 1);
}

void render(uint32_t now)
{
	if (sprite_ready) {
		draw_ui(frame, now, true);
		frame.pushSprite(0, 0);
	} else {
		// Keep the display usable even when the heap cannot fit a sprite.
		draw_ui(tft, now, false);
	}
}

void draw_splash()
{
	// One indexed color image in flash; one RGB565 row on the stack.
	uint16_t row[BootSplash::width];
	tft.setSwapBytes(true);
	for (int y = 0; y < BootSplash::height; ++y) {
		for (int x = 0; x < BootSplash::width; ++x) {
			const uint8_t index = pgm_read_byte(
				&BootSplash::pixels[y * BootSplash::width + x]);
			row[x] = pgm_read_word(&BootSplash::palette[index]);
		}
		tft.pushImage(0, y, BootSplash::width, 1, row);
	}
	tft.setSwapBytes(false);
}
} // namespace

void setup()
{
	Serial.begin(115200);
	WiFi.mode(WIFI_OFF);
	btStop();
	pinMode(0, INPUT_PULLUP);
	for (const int pin : {4, 16, 17}) {
		pinMode(pin, OUTPUT);
		digitalWrite(pin, HIGH);
	}
	gps_uart.setRxBufferSize(2048);
	gps_uart.begin(Config::gps_baud, SERIAL_8N1, Config::gps_rx, Config::gps_tx);
	tft.init();
	tft.setRotation(Config::rotation);
	tft.fillScreen(TFT_BLACK);
	ledcSetup(Config::backlight_channel, 5000, 8);
	ledcAttachPin(TFT_BL, Config::backlight_channel);
	set_brightness();
	// Allocate before SD initialization can fragment the heap.
	frame.setColorDepth(4);
	sprite_ready = frame.createSprite(tft.width(), tft.height()) != nullptr;
	if (sprite_ready) {
		for (uint8_t i = 0; i < sizeof(ui_palette) / sizeof(ui_palette[0]); ++i)
			frame.setPaletteColor(i, ui_palette[i]);
	} else {
		Serial.println("WARNING: sprite unavailable; using direct LCD drawing");
	}
	track.begin();
	if (Config::resistive_touch_enabled) {
		touch_spi.begin(25, 39, 32, 33);
		touch.begin(touch_spi);
	}
	Serial.println("GPS speedometer: 9600 baud default, RX GPIO35; no ECU interface");
	draw_splash();
	splash_start_ms = millis();
	splash_visible = true;
}

void loop()
{
	poll_gps();
	const uint32_t now = millis();
	poll_controls(now);
	track.poll(now);
	if (splash_visible && now - splash_start_ms >= Config::splash_ms) {
		splash_visible = false;
		tft.fillScreen(TFT_BLACK);
	}
	if (!splash_visible && now - last_render_ms >= Config::render_ms) {
		last_render_ms = now;
		render(now);
	}
	if (now - last_diagnostics_ms >= Config::diagnostics_ms) {
		last_diagnostics_ms = now;
		Serial.printf("bytes=%lu checksum_ok=%lu checksum_bad=%lu fresh=%d speed=%.1f trip=%.3f\n",
			      static_cast<unsigned long>(byte_count),
			      gps.passedChecksum(), gps.failedChecksum(),
			      speed.fresh(now), speed.speed(), speed.trip());
	}
	delay(1);  // Yield to RTOS; no blocking GPS wait or acquisition screen.
}
#endif
