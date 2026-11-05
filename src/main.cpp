/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp32-hal-cpu.h>

#include "config.h"
#include "firmware_version.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_redraw_ms = 0;
bool g_was_wifi_paused = false;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_last_redraw_ms = millis(); // Avoid another full redraw immediately.
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\r\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (!wifiRadarWorkPaused() && g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
    g_last_redraw_ms = millis();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    onRangeTap();
  }
}

// ADS-B fetch runs in its own FreeRTOS task. The ESP32-C3 has one CPU core,
// so these tasks share CPU time, but a blocking HTTPS fetch no longer blocks
// the main web-server loop directly. Aircraft snapshots remain mutex-protected.
void adsbFetchTask(void*) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && !wifiRadarWorkPaused()) {
      // Serialize the full TLS/JSON fetch against web settings commits.
      // If busy, retry shortly without skipping a five-second fetch period.
      if (wifiTryBeginAdsbFetch()) {
        // Capture memory both before and AFTER fetchUpdate returns (when the
        // TLS/HTTP and JSON temporary objects have been destroyed). This helps
        // distinguish retained allocations from a short-lived TLS peak.
        const size_t heap_before = ESP.getFreeHeap();
        const size_t block_before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        const bool ok = services::adsb::fetchUpdate(services::location::lat(),
                                                     services::location::lon(),
                                                     ui::radar::fetchRadiusKm());
        static uint32_t last_memory_log_ms = 0;
        const uint32_t now = millis();
        if (!ok || static_cast<uint32_t>(now - last_memory_log_ms) >= 300000u) {
          const size_t heap_after = ESP.getFreeHeap();
          const size_t block_after = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
          Serial.printf("adsb memory (%s): free %u -> %u, largest %u -> %u, min %u bytes\r\n",
                        ok ? "ok" : "failed",
                        static_cast<unsigned>(heap_before),
                        static_cast<unsigned>(heap_after),
                        static_cast<unsigned>(block_before),
                        static_cast<unsigned>(block_after),
                        static_cast<unsigned>(ESP.getMinFreeHeap()));
          last_memory_log_ms = now;
        }
        wifiEndAdsbFetch();
        vTaskDelay(pdMS_TO_TICKS(config::kAdsbFetchIntervalMs));
      } else {
        // A web save is in progress, or needs to retry after TLS finishes.
        vTaskDelay(pdMS_TO_TICKS(250));
      }
    } else {
      // Wake frequently while paused so normal reception resumes promptly.
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.printf("Plane Radar %s\r\n", firmware::kVersion);
  // This device is continuously powered. Avoid deliberate CPU clock reduction;
  // Wi-Fi TX power remains limited independently to 8.5 dBm for stability.
  const bool clock_ok = setCpuFrequencyMhz(160);
  Serial.printf("CPU fixed target: 160 MHz, reported=%lu MHz (%s)\r\n",
                static_cast<unsigned long>(getCpuFrequencyMhz()),
                clock_ok ? "configured" : "set failed");

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::adsb::init();

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }

  // Background ADS-B fetch, pinned to core 0 (the C3's single core).
  // It checks Wi-Fi state each cycle.
  xTaskCreatePinnedToCore(adsbFetchTask, "adsb", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
  handleBootButton();
  wifiLoop(); // Must ALWAYS keep serving HTTP while Wi-Fi settings are open.

  const bool wifi_paused = wifiRadarWorkPaused();
  if (wifi_paused) {
    // Keep the last frame visible; do not paint, redraw, or start a reconnect.
    // A strict watchdog in wifi_setup.cpp guarantees automatic release.
    g_was_wifi_paused = true;
    delay(10);
    return;
  }
  if (g_was_wifi_paused) {
    g_was_wifi_paused = false;
    ui::radarDisplayInvalidateSlowLayer();
    // Recover a clean scene after network scanning or page navigation.
    g_last_redraw_ms = 0;
    Serial.println("Radar drawing and ADS-B resumed after Wi-Fi setup");
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (millis() - g_last_redraw_ms >= ui::radar::redrawIntervalMs() &&
               !wifiDisplayYieldForWeb()) {
      // Per-range full-frame cadence (2..6.25 FPS). Web work stays first;
      // ADS-B continues independently at the same 5-second fetch interval.
      const uint32_t interval_ms = ui::radar::redrawIntervalMs();
      const uint32_t frame_started = millis();
      ui::radarDisplayRefreshAircraft();
      const uint32_t elapsed = millis() - frame_started;
      // Never queue overdue frames. If rendering exceeded its interval, allow
      // one complete interval after that slow frame for Wi-Fi/HTTP/ADS-B.
      g_last_redraw_ms = elapsed >= interval_ms ? millis() : frame_started;
    }
  }

  delay(10);
}
