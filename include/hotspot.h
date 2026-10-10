#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>
#include <atomic>
#if __has_include("wifi_credentials.local.h")
#include "wifi_credentials.local.h"
#endif

// Credentials stay in device NVS, never in source or serial logs.
class Hotspot {
public:
	void begin()
	{
		preferences_.begin("palacita-wifi", false);
		WiFi.persistent(false);
		WiFi.mode(WIFI_STA);
		// Own the retry timer, including initial failures and hotspot restarts.
		WiFi.setAutoReconnect(false);
		WiFi.setSleep(false);
		WiFi.onEvent([this](WiFiEvent_t, WiFiEventInfo_t info) {
			disconnect_reason_.store(info.wifi_sta_disconnected.reason);
			report_disconnect_.store(true);
		}, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
		ssid_ = preferences_.getString("ssid", "");
		String password = preferences_.getString("password", "");
#ifdef PALACITA_WIFI_SSID
		// A local build override replaces previously provisioned credentials.
		ssid_ = PALACITA_WIFI_SSID;
		password = PALACITA_WIFI_PASSWORD;
		if (preferences_.getString("ssid", "") != ssid_ ||
		    preferences_.getString("password", "") != password) {
			preferences_.putString("ssid", ssid_);
			preferences_.putString("password", password);
		}
#endif
		if (ssid_.length()) connect(password);
		else portal();
		configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
	}
	void poll(uint32_t now)
	{
		if (portal_on_) server_.handleClient();
		if (report_disconnect_.exchange(false))
			Serial.printf("WiFi disconnected: reason=%u (%s)\n",
				      disconnect_reason_.load(), failure());
		if (WiFi.status() == WL_CONNECTED) {
			if (!connected_) {
				Serial.printf("WiFi connected: IP=%s channel=%d RSSI=%d\n",
					      WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());
				connected_ = true;
				disconnect_reason_.store(0);
			}
			if (portal_on_) {
				server_.stop();
				WiFi.softAPdisconnect(true);
				WiFi.mode(WIFI_STA);
				portal_on_ = false;
			}
			return;
		}
		if (connected_) {
			connected_ = false;
			attempt_ms_ = now - 15000;
		}
		if (ssid_.length() && now - attempt_ms_ >= 15000) {
			// Keep a configured hotspot in station mode to avoid AP/channel churn.
			WiFi.disconnect(false, false);
			connect(password_);
		}
	}
	bool setup_active() const { return portal_on_; }
	const char *setup_password() const { return ap_password_; }
	const char *status() const
	{
		if (WiFi.status() == WL_CONNECTED) return "WiFi connected";
		if (portal_on_) return "WiFi setup";
		return disconnect_reason_.load() ? failure() : "WiFi connecting";
	}
private:
	const char *failure() const
	{
		switch (disconnect_reason_.load()) {
		case WIFI_REASON_NO_AP_FOUND: return "Hotspot not found";
		case WIFI_REASON_AUTH_FAIL: return "WiFi auth failed";
		case WIFI_REASON_HANDSHAKE_TIMEOUT:
		case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "WiFi handshake failed";
		case WIFI_REASON_ASSOC_FAIL: return "Hotspot rejected WiFi";
		default: return "WiFi retrying";
		}
	}
	void connect(const String &password)
	{
		password_ = password;
		Serial.printf("WiFi connecting to '%s' (2.4 GHz); retry every 15s\n", ssid_.c_str());
		WiFi.begin(ssid_.c_str(), password.c_str());
		attempt_ms_ = millis();
	}
	void portal()
	{
		if (portal_on_) return;
		snprintf(ap_password_, sizeof(ap_password_), "%08lu",
			 static_cast<unsigned long>(esp_random() % 100000000));
		WiFi.mode(WIFI_AP_STA);
		if (!WiFi.softAP("PALACITA-SETUP", ap_password_)) return;
		server_.on("/", HTTP_GET, [this]() {
			server_.send(200, "text/html", "<!doctype html><meta name='viewport' content='width=device-width'><title>PALACITA WiFi</title><h1>Connect to your 2.4 GHz hotspot</h1><form method='post' action='/connect'><label>SSID <input name='ssid' maxlength='32' required></label><br><label>Password <input name='password' type='password' maxlength='63'></label><br><button>Connect</button></form>");
		});
		server_.on("/connect", HTTP_POST, [this]() {
			String ssid = server_.arg("ssid"), password = server_.arg("password");
			if (!ssid.length() || ssid.length() > 32 ||
			    (password.length() && (password.length() < 8 || password.length() > 63))) {
				server_.send(400, "text/plain", "SSID: 1-32 bytes; password: 8-63 bytes or empty for open network.");
				return;
			}
			ssid_ = ssid;
			preferences_.putString("ssid", ssid);
			preferences_.putString("password", password);
			server_.send(200, "text/plain", "Connecting. Check MAP. Setup stays available if connection fails.");
			connect(password);
		});
		server_.begin();
		portal_on_ = true;
	}
	Preferences preferences_;
	WebServer server_{80};
	String ssid_;
	String password_;
	std::atomic<unsigned> disconnect_reason_{0};
	std::atomic<bool> report_disconnect_{false};
	bool connected_ = false;
	char ap_password_[9] = "";
	uint32_t attempt_ms_ = 0;
	bool portal_on_ = false;
};
