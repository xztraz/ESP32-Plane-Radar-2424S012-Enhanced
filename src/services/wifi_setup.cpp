#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiManager.h>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <memory>
#include <new>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp32-hal-cpu.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "firmware_version.h"
#include "services/radar_location.h"
#include "services/aircraft_style_settings.h"
#include "services/aircraft_watchlist.h"
#include "ui/radar_range.h"
#include "ui/aircraft_altitude_units.h"
#include "ui/aircraft_trail_settings.h"
#include "ui/radar_display_options.h"
#include "ui/status_screens.h"
#include "ui/radar_display.h"
#include "services/adsb_client.h"
#include "services/radar_web_pages.h"
#include "services/fetch_save_gate.h"
#include "services/web_health_reply.h"
#include <WebServer.h>

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
volatile uint8_t s_boot_reset_seconds = 0;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;
// Port 80 belongs to an independent WebServer, not WiFiManager.
// Check a *complete* /health HTTP response, cooperatively: the request is
// sent in one main-loop iteration, WebServer::handleClient() serves it in
// later iterations, and polling only consumes bytes that are already ready.
// A synchronous GET from this loop would deadlock/stall the server.
constexpr uint32_t kWebProbeIntervalMs = 45000u;
constexpr uint32_t kWebStartupGraceMs = 15000u;
constexpr uint32_t kWebProbeResponseTimeoutMs = 4000u;
constexpr uint32_t kWebRestartCooldownMs = 120000u;
constexpr uint8_t kWebProbeFailuresBeforeRestart = 3;
constexpr uint16_t kWifiAdminPort = 81;
std::unique_ptr<WebServer> s_lan_server;
WebServer* s_current_http_server = nullptr;
bool s_wifi_admin_requested = false;
bool s_wifi_admin_close_requested = false;
uint32_t s_web_started_ms = 0;
uint32_t s_web_last_probe_ms = 0;
uint8_t s_web_probe_failures = 0;
uint32_t s_web_last_start_attempt_ms = 0;
uint32_t s_web_last_rebuild_ms = 0;
uint32_t s_web_health_ok_total = 0;
uint32_t s_web_health_fail_total = 0;
uint32_t s_web_health_rebuild_total = 0;
uint32_t s_web_health_verified_recovery_total = 0;
uint32_t s_web_last_verified_recovery_ms = 0;
bool s_web_verify_after_rebuild = false;
uint32_t s_web_health_handler_hits = 0;
uint32_t s_web_probe_handler_hits_at_start = 0;
uint32_t s_web_probe_started_ms = 0;
enum class WebProbeState : uint8_t { kIdle, kAwaitingResponse };
WebProbeState s_web_probe_state = WebProbeState::kIdle;
WiFiClient s_web_probe_client;
services::web_health::Reply s_web_probe_reply;

// Bounded, non-blocking Wi-Fi work window. The main loop continues
// serving HTTP and the BOOT button; only radar painting and new ADS-B HTTPS
// transfers are paused. Atomic flags coordinate the ADS-B FreeRTOS task.
constexpr uint32_t kWifiPauseWindowMs = 45000UL;
constexpr uint32_t kWifiPauseHardLimitMs = 120000UL;
std::atomic<uint32_t> s_wifi_pause_started_ms{0};
std::atomic<uint32_t> s_wifi_pause_until_ms{0};
std::atomic<bool> s_adsb_fetch_active{false};
services::FetchSaveGate s_fetch_save_gate;

bool radarWorkPausedInternal() {
  const uint32_t until = s_wifi_pause_until_ms.load();
  if (!until) return false;
  const uint32_t now = millis();
  const uint32_t started = s_wifi_pause_started_ms.load();
  return static_cast<int32_t>(until - now) > 0 &&
         static_cast<uint32_t>(now - started) < kWifiPauseHardLimitMs;
}

void beginWifiWork() {
  s_wifi_admin_requested = true;
  s_wifi_admin_close_requested = false;
  const uint32_t now = millis();
  if (!radarWorkPausedInternal()) {
    s_wifi_pause_started_ms.store(now);
    Serial.println("Wi-Fi setup: pausing radar rendering and new ADS-B requests");
  }
  s_wifi_pause_until_ms.store(now + kWifiPauseWindowMs);
}

void keepWifiWorkAlive() {
  if (radarWorkPausedInternal()) {
    s_wifi_pause_until_ms.store(millis() + kWifiPauseWindowMs);
  }
}

void finishWifiWork() {
  if (radarWorkPausedInternal()) Serial.println("Wi-Fi setup: radar resumes");
  s_wifi_pause_until_ms.store(0);
  if (s_wifi_admin_requested) s_wifi_admin_close_requested = true;
}

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

// Keep WiFiManager only for Wi-Fi credentials and captive-portal
// handling. Radar pages are independent, statically-served routes. The V16
// custom WiFiManager parameter form used to concatenate ~4 KB watchlist state,
// style settings, and a large JavaScript UI into every /param response.
// WiFiManager 2.0.x has setCustomHeadElement(), but no
// setCustomBodyHeader(). Add a tiny navigation bar to its Wi-Fi pages after
// DOMContentLoaded; radar pages have their own navigation in flash-resident
// HTML. Do not allocate a large dynamic string for this menu.
constexpr char kWifiNavigationHead[] = R"radarnav(<script>
(function(){var c=document.createElement('link');c.rel='stylesheet';c.href='http://'+location.hostname+'/assets/radar.css';document.head.appendChild(c);})();
document.addEventListener('DOMContentLoaded', function () {
  document.body.classList.add('plane-radar-wifi');
  if (document.getElementById('plane-radar-links')) return;
  var menu = document.createElement('nav');
  menu.id = 'plane-radar-links';
  menu.setAttribute('aria-label', 'Plane Radar navigation');
  [['General','/general'],['Aircraft','/aircraft'],['Wi-Fi','/wifi-settings'],['Status','/status']]
    .forEach(function (item) {
      var link = document.createElement('a');
      link.href = 'http://' + location.hostname + item[1];
      link.textContent = item[0];
      if (item[0] === 'Wi-Fi') link.className = 'active';
      menu.appendChild(link);
    });
  if (document.body) document.body.insertBefore(menu, document.body.firstChild);
  // WiFiManager retains control of the scan and credential forms.
  if (location.pathname === '/wifi' || location.pathname === '/wifinoscan') {
    setInterval(function () {
      if (document.visibilityState === 'visible')
        fetch('http://' + location.hostname + '/api/wifi/ping', {mode:'no-cors', cache:'no-store'}).catch(function () {});
    }, 10000);
  }
});
</script>)radarnav";

// The Wi-Fi tab is lightweight. Opening it never starts a scan or allocates
// network lists. Users opt into scanning or manual network configuration.
const char kWifiSettingsHtml[] PROGMEM = R"rdr(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Plane Radar - Wi-Fi</title><link rel="stylesheet" href="/assets/radar.css"></head>
<body><h1>Plane Radar</h1><nav><a href="/general">General</a><a href="/aircraft">Aircraft</a><a href="/wifi-settings" class="active">Wi-Fi</a><a href="/status">Status</a></nav>
<section><h2>Wi-Fi settings</h2><p>Connected network: <strong id="ssid">Loading...</strong></p>
<p>Transmit power: 8.5 dBm (unchanged).</p>
<p>Network scanning uses additional RAM. It is started only when you request it; aircraft drawing and new ADS-B downloads pause briefly during configuration.</p>
<p><a href="/wifi-prepare">Scan available Wi-Fi networks</a></p>
<p><a href="/wifi-prepare?manual=1">Enter network name manually (no scan)</a></p>
<p><a href="/general">Return to radar settings</a></p></section>
<script>fetch('/api/wifi/ssid',{cache:'no-store'}).then(function(r){return r.text();}).then(function(s){document.getElementById('ssid').textContent=s || '(not connected)';}).catch(function(){document.getElementById('ssid').textContent='Unavailable';});</script>
</body></html>)rdr";

const char kWifiPrepareHtml[] PROGMEM = R"rdr(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Plane Radar - Wi-Fi</title><link rel="stylesheet" href="/assets/radar.css"></head>
<body><h1>Plane Radar</h1><nav><a href="/general">General</a><a href="/aircraft">Aircraft</a><a href="/wifi-settings" class="active">Wi-Fi</a><a href="/status">Status</a></nav>
<section><h2>Wi-Fi configuration</h2><p id="message">Pausing radar and finishing the current aircraft download before scanning for networks...</p>
<p><a href="/general">Cancel and resume radar</a></p>
<div id="fallback" hidden><p>Aircraft download did not finish promptly. To avoid locking up the radar, a network scan was not started.</p>
<p><a href="#" id="manual-fallback">Enter network manually (without a scan)</a></p>
<p><a href="/wifi-prepare">Retry network scan</a></p></div></section>
<script>
(function () {
  var began = Date.now();
  var wifiAdminBase = 'http://' + location.hostname + ':81';
  document.getElementById('manual-fallback').href = wifiAdminBase + '/wifinoscan';
  function check() {
    fetch('/api/wifi/ready', {cache:'no-store'}).then(function (r) {return r.json();}).then(function (s) {
      if (s.ready) {location.replace(wifiAdminBase + (location.search.indexOf('manual=1') >= 0 ? '/wifinoscan' : '/wifi')); return;}
      if (Date.now()-began > 15000) {document.getElementById('message').textContent='The radar is still busy.';document.getElementById('fallback').hidden=false;return;}
      setTimeout(check,500);
    }).catch(function () {
      if (Date.now()-began < 15000) setTimeout(check,700);
      else {document.getElementById('message').textContent='Wi-Fi configuration is temporarily unavailable.';document.getElementById('fallback').hidden=false;}
    });
  }
  check();
})();
</script></body></html>)rdr";

uint32_t s_last_web_us = 0;
// A short, non-blocking pause after serving a radar web response gives the
// browser time to load settings/assets without competing with GC9A01 drawing.
// HTTP callbacks run on main, and wifiLoop() stays first in every iteration.
constexpr uint32_t kWebDisplayYieldMs = 650;
std::atomic<uint32_t> s_last_web_served_ms{0};

void noteWebResponse() {
  s_last_web_served_ms.store(millis(), std::memory_order_relaxed);
}


void sendStatic(const char* contents, const char* content_type) {
  // Flash-resident static responses: no large temporary String allocations.
  // Avoid serving stale JS/CSS after a firmware update while debugging portal
  // compatibility. We can enable immutable versioned asset caching later.
  s_current_http_server->sendHeader("Cache-Control", "no-store");
  s_current_http_server->send_P(200, content_type, contents);
  noteWebResponse();
}

void sendNoCache(int status, const char* type, const String& body,
                 bool defer_rendering = true) {
  s_current_http_server->sendHeader("Cache-Control", "no-store");
  s_current_http_server->send(status, type, body);
  if (defer_rendering) noteWebResponse();
}

// All web POST handlers run on the single WiFiManager loop. The ADS-B
// connection runs on a separate task. Serialize *writes* against the full
// HTTPS request so large TLS allocations never overlap NVS commits.
// If TLS is active, return immediately and let the page retry after a delay.
class SaveGuard {
 public:
  SaveGuard() : acquired_(s_fetch_save_gate.tryBeginSave(millis())) {}
  ~SaveGuard() { if (acquired_) s_fetch_save_gate.endSave(millis()); }
  bool acquired() const { return acquired_; }
  SaveGuard(const SaveGuard&) = delete;
  SaveGuard& operator=(const SaveGuard&) = delete;
 private:
  bool acquired_;
};

void sendSaveBusy() {
  s_current_http_server->sendHeader("Retry-After", "1");
  sendNoCache(503, "text/plain", "ADS-B download busy; save will retry");
}

bool isChecked(const String& val) { return val == "1"; }

void handleGeneralGet() {
  const uint32_t start = micros();
  char response[224];
  snprintf(response, sizeof(response),
           "{\"lat\":%.6f,\"lon\":%.6f,\"range\":%u,\"bgtheme\":%u,\"bgpattern\":%u,\"miles\":%s,"
           "\"runways\":%s,\"metres\":%s}",
           services::location::lat(), services::location::lon(),
           static_cast<unsigned>(ui::radar::rangeIndex()),
           static_cast<unsigned>(ui::radar::backgroundTheme()),
           static_cast<unsigned>(ui::radar::backgroundPattern()),
           ui::radar::useMiles() ? "true" : "false",
           ui::radar::showRunways() ? "true" : "false",
           ui::altitude_units::meters() ? "true" : "false");
  // Diagnostics polling must not pause radar rendering for 650 ms.
  sendNoCache(200, "application/json", response, false);
  s_last_web_us = micros() - start;
}

void handleGeneralPost() {
  const uint32_t start = micros();
  if (!s_current_http_server->hasArg("lat") || !s_current_http_server->hasArg("lon") ||
      !s_current_http_server->hasArg("range") || !s_current_http_server->hasArg("bgtheme") ||
      !s_current_http_server->hasArg("bgpattern")) {
    sendNoCache(400, "text/plain", "Missing location/range"); return;
  }
  const String index_text = s_current_http_server->arg("range");
  if (index_text.length() != 1 || index_text[0] < '0' ||
      index_text[0] >= '0' + ui::radar::kRangePresetCount) {
    sendNoCache(400, "text/plain", "Invalid radar range"); return;
  }
  const String lat = s_current_http_server->arg("lat");
  const String lon = s_current_http_server->arg("lon");
  const String bgtheme = s_current_http_server->arg("bgtheme");
  const String bgpattern = s_current_http_server->arg("bgpattern");
  if (bgtheme.length() != 1 || bgtheme[0] < '0' || bgtheme[0] > '5' ||
      bgpattern.length() != 1 || bgpattern[0] < '0' || bgpattern[0] > '5') {
    sendNoCache(400, "text/plain", "Invalid radar color/pattern"); return;
  }
  SaveGuard save_guard;
  if (!save_guard.acquired()) { sendSaveBusy(); return; }
  if (!services::location::saveFromStrings(lat.c_str(), lon.c_str())) {
    sendNoCache(400, "text/plain", "Invalid latitude/longitude"); return;
  }
  ui::radar::rangeSetIndex(static_cast<uint8_t>(index_text[0] - '0'));
  ui::radar::saveBackgroundTheme(static_cast<uint8_t>(bgtheme[0] - '0'));
  ui::radar::saveBackgroundPattern(static_cast<uint8_t>(bgpattern[0] - '0'));
  ui::radar::saveMilesFromPortal(isChecked(s_current_http_server->arg("miles")) ? "T" : "");
  ui::radar::saveRunwaysFromPortal(isChecked(s_current_http_server->arg("runways")) ? "T" : "");
  ui::altitude_units::save(isChecked(s_current_http_server->arg("metres")) ? "m" : "ft");
  ui::radarDisplayThemeChanged();
  sendNoCache(200, "text/plain", "Saved");
  s_last_web_us = micros() - start;
}

void handleAircraftGet() {
  const uint32_t start = micros();
  // The watchlist string is only generated when this particular page opens.
  String result;
  result.reserve(4300);
  result += services::aircraft_styles::encoded();
  result += '\n';
  result += ui::display_options::encoded();
  result += '\n';
  result += String(ui::trails::points());
  result += '\n';
  result += services::watchlist::encoded();
  result += '\n';
  result += String(ui::trails::startDarkeningPercent());
  result += '\n';
  result += String(ui::trails::fadeCurve());
  result += '\n';
  result += String(ui::trails::dotSpacing());
  result += '\n';
  result += String(ui::trails::dotDiameterHalfPx());
  sendNoCache(200, "text/plain; charset=utf-8", result);
  s_last_web_us = micros() - start;
}

void handleAircraftPost() {
  const uint32_t start = micros();
  auto* server = s_current_http_server;
  if (!server->hasArg("colors") || !server->hasArg("options") ||
      !server->hasArg("trail") || !server->hasArg("trailDark") ||
      !server->hasArg("trailCurve") || !server->hasArg("dotSpacing") ||
      !server->hasArg("dotSize") || !server->hasArg("watchlist")) {
    sendNoCache(400, "text/plain", "Missing aircraft settings"); return;
  }
  const String colors = server->arg("colors");
  const String options = server->arg("options");
  const String trail = server->arg("trail");
  const String trailDark = server->arg("trailDark");
  const String trailCurve = server->arg("trailCurve");
  const String dotSpacing = server->arg("dotSpacing");
  const String dotSize = server->arg("dotSize");
  const String watches = server->arg("watchlist");
  if (colors.length() > 48 || options.length() > 24 ||
      trail.length() > 3 || trailDark.length() > 3 ||
      trailCurve.length() > 2 || dotSpacing.length() > 2 ||
      dotSize.length() > 2 || watches.length() > 3700) {
    sendNoCache(400, "text/plain", "Aircraft settings too long"); return;
  }
  // Keep web settings writes out of the TLS handshake/parse lifetime.
  SaveGuard save_guard;
  if (!save_guard.acquired()) { sendSaveBusy(); return; }
  // Each save validates its own input before touching NVS, retaining existing
  // values when malformed. The browser submits all aircraft settings at once.
  if (!services::aircraft_styles::save(colors.c_str())) {
    sendNoCache(400, "text/plain", "Invalid aircraft colors/priority"); return;
  }
  if (!ui::display_options::save(options.c_str())) {
    sendNoCache(400, "text/plain", "Invalid label/icon options"); return;
  }
  if (!ui::trails::saveFromPortal(trail.c_str())) {
    sendNoCache(400, "text/plain", "Invalid trail points"); return;
  }
  if (!ui::trails::saveStartDarkeningPercent(trailDark.c_str())) {
    sendNoCache(400, "text/plain", "Invalid trail start darkening"); return;
  }
  if (!ui::trails::saveFadeCurve(trailCurve.c_str())) {
    sendNoCache(400, "text/plain", "Invalid trail fade curve"); return;
  }
  if (!ui::trails::saveDotSpacing(dotSpacing.c_str())) {
    sendNoCache(400, "text/plain", "Invalid dot spacing"); return;
  }
  if (!ui::trails::saveDotDiameterHalfPx(dotSize.c_str())) {
    sendNoCache(400, "text/plain", "Invalid dot diameter"); return;
  }
  if (!services::watchlist::save(watches.c_str())) {
    sendNoCache(400, "text/plain", "Invalid watchlist"); return;
  }
  ui::radarDisplayInvalidateSlowLayer();
  sendNoCache(200, "text/plain", "Saved");
  s_last_web_us = micros() - start;
}

void handleStatusGet() {
  const uint32_t start = micros();
  char response[1050];
  const ui::RadarFrameTimings timing = ui::radarDisplayFrameTimings();
  const ui::RadarSpiSlowDiag slow_spi = ui::radarDisplaySpiSlowDiag();
  const unsigned long last = services::adsb::lastUpdateMs();
  char age[22];
  if (!last) {
    strcpy(age, "null");
  } else {
    snprintf(age, sizeof(age), "%lu", millis() - last);
  }
  char last_verified_recovery_age[24];
  if (!s_web_health_verified_recovery_total) {
    strcpy(last_verified_recovery_age, "null");
  } else {
    snprintf(last_verified_recovery_age, sizeof(last_verified_recovery_age), "%lu",
             static_cast<unsigned long>((millis() - s_web_last_verified_recovery_ms) / 1000u));
  }
  const unsigned long long uptime_sec =
      static_cast<unsigned long long>(esp_timer_get_time() / 1000000LL);
  const int response_len = snprintf(response, sizeof(response),
      "{\"version\":\"%s\",\"rssi\":%d,\"heap\":%lu,\"minheap\":%lu,\"count\":%u,"
      "\"fetchAgeMs\":%s,\"frameIntervalMs\":%u,\"renderUs\":%lu,\"slowUs\":%lu,\"webUs\":%lu,\"frames\":%u,\"largestHeap\":%lu,"
      "\"gridUs\":%lu,\"runwaysUs\":%lu,\"projectionUs\":%lu,"
      "\"trailsUs\":%lu,\"dotsUs\":%lu,\"labelsUs\":%lu,"
      "\"iconsUs\":%lu,\"transferUs\":%lu,"
      "\"flashGrid\":%u,\"flashCopyUs\":%lu,"
      "\"cpuMhz\":%lu,\"frameCpuStart\":%u,\"frameCpuEnd\":%u,"
      "\"slowSpiCount\":%lu,\"slowSpiCpuStart\":%u,\"slowSpiCpuEnd\":%u,\"slowSpiUs\":%lu,"
      "\"uptimeSec\":%llu,\"webHealthOk\":%lu,\"webHealthFailed\":%lu,"
      "\"webRebuilds\":%lu,\"webVerifiedRecoveries\":%lu,"
      "\"webLastRecoveryAgeSec\":%s}",
      firmware::kVersion, WiFi.RSSI(), static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getMinFreeHeap()),
      static_cast<unsigned>(services::adsb::aircraftCount()), age,
      static_cast<unsigned>(ui::radar::redrawIntervalMs()),
      static_cast<unsigned long>(ui::radarDisplayLastRenderUs()),
      static_cast<unsigned long>(ui::radarDisplayLastSlowRenderUs()),
      static_cast<unsigned long>(s_last_web_us),
      static_cast<unsigned>(ui::radarDisplayFramebufferCount()),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
      static_cast<unsigned long>(timing.grid_us),
      static_cast<unsigned long>(timing.runways_us),
      static_cast<unsigned long>(timing.projection_us),
      static_cast<unsigned long>(timing.trails_us),
      static_cast<unsigned long>(timing.dots_us),
      static_cast<unsigned long>(timing.labels_us),
      static_cast<unsigned long>(timing.icons_us),
      static_cast<unsigned long>(timing.transfer_us),
      static_cast<unsigned>(ui::radarDisplayFlashGridActive()),
      static_cast<unsigned long>(timing.flash_copy_us),
      static_cast<unsigned long>(getCpuFrequencyMhz()),
      static_cast<unsigned>(timing.cpu_mhz_frame_start),
      static_cast<unsigned>(timing.cpu_mhz_frame_end),
      static_cast<unsigned long>(slow_spi.count),
      static_cast<unsigned>(slow_spi.cpu_mhz_start),
      static_cast<unsigned>(slow_spi.cpu_mhz_end),
      static_cast<unsigned long>(slow_spi.last_transfer_us),
      uptime_sec,
      static_cast<unsigned long>(s_web_health_ok_total),
      static_cast<unsigned long>(s_web_health_fail_total),
      static_cast<unsigned long>(s_web_health_rebuild_total),
      static_cast<unsigned long>(s_web_health_verified_recovery_total),
      last_verified_recovery_age);
  if (response_len < 0 || static_cast<size_t>(response_len) >= sizeof(response)) {
    sendNoCache(500, "text/plain", "Status response too large", false);
    return;
  }
  // Diagnostics polling must not pause radar rendering for 650 ms.
  sendNoCache(200, "application/json", response, false);
  s_last_web_us = micros() - start;
}

void bindRadarRoutes(WebServer& server) {
  server.on("/wifi-settings", HTTP_GET, []() {
    if (wifiLinkUp()) {
      finishWifiWork();
      sendStatic(kWifiSettingsHtml, "text/html; charset=utf-8");
    } else {
      s_current_http_server->sendHeader("Location", "/wifi");
      s_current_http_server->send(302, "text/plain", "Wi-Fi setup");
    }
  });
  server.on("/api/wifi/ssid", HTTP_GET, []() {
    sendNoCache(200, "text/plain; charset=utf-8", WiFi.SSID());
  });
  // Wi-Fi prep never performs a scan in the HTTP callback. The existing
  // WiFiManager /wifi endpoint is opened only after ADS-B TLS is idle.
  server.on("/wifi-prepare", HTTP_GET, []() {
    if (wifiLinkUp()) {
      beginWifiWork();
      sendStatic(kWifiPrepareHtml, "text/html; charset=utf-8");
    } else {
      // During captive AP setup, the radar is not fetching data yet.
      s_current_http_server->sendHeader("Location", "/wifi");
      s_current_http_server->send(302, "text/plain", "Wi-Fi setup");
    }
  });
  server.on("/api/wifi/ready", HTTP_GET, []() {
    char result[80];
    const bool active = radarWorkPausedInternal();
    const bool fetching = s_adsb_fetch_active.load();
    snprintf(result, sizeof(result),
             "{\"paused\":%s,\"fetching\":%s,\"ready\":%s}",
             active ? "true" : "false", fetching ? "true" : "false",
             (active && !fetching && s_wm.getWebPortalActive()) ? "true" : "false");
    sendNoCache(200, "application/json", result);
  });
  server.on("/api/wifi/ping", HTTP_GET, []() {
    keepWifiWorkAlive();
    sendNoCache(200, "text/plain", "OK");
  });
  server.on("/api/wifi/resume", HTTP_GET, []() {
    finishWifiWork();
    sendNoCache(200, "text/plain", "OK");
  });
  // Called by WiFiManager when its server has started (LAN or captive AP).
  // All handlers run in wifiLoop() on the main loop, no second webserver/task.
  // Register this BEFORE WiFiManager adds its own root route. Its embedded
  // webserver uses the first matching route. Do not allow a generic captive-
  // portal landing page to substitute an unusable address for the LAN home.
  // The LAN home serves General directly: no DNS/mDNS or absolute redirects.
  server.on("/", HTTP_GET, []() {
    finishWifiWork();
    if (wifiLinkUp()) {
      sendStatic(services::radar_web_pages::kGeneral,
                 "text/html; charset=utf-8");
    } else {
      // During first-time setup, continue exposing WiFiManager's /wifi route.
      s_current_http_server->sendHeader("Cache-Control", "no-store");
      s_current_http_server->send(200, "text/html; charset=utf-8",
          "<!doctype html><html><head><meta name='viewport' "
          "content='width=device-width,initial-scale=1'>"
          "<title>Plane Radar Setup</title>"
          "<link rel='stylesheet' href='/assets/radar.css'></head><body>"
          "<h1>Plane Radar</h1>"
          "<nav><a href='/general'>General</a><a href='/aircraft'>Aircraft</a>"
          "<a href='/wifi' class='active'>Wi-Fi</a><a href='/status'>Status</a></nav>"
          "<section><h2>Initial Wi-Fi setup</h2>"
          "<p>Connect this radar to your Wi-Fi network.</p>"
          "<p><a href='/wifi'>Configure Wi-Fi</a></p>"
          "</section></body></html>");
    }
  });
  // Ultra-lightweight test: distinguishes name-resolution / routing errors
  // from a busy server and works without loading any JavaScript.
  server.on("/health", HTTP_GET, []() {
    ++s_web_health_handler_hits;
    s_current_http_server->sendHeader("Cache-Control", "no-store");
    s_current_http_server->send(200, "text/plain; charset=utf-8",
                      "Plane Radar web server OK\n");
    noteWebResponse();
  });
  server.on("/general", HTTP_GET, []() {
    finishWifiWork();
    sendStatic(services::radar_web_pages::kGeneral, "text/html; charset=utf-8");
  });
  server.on("/aircraft", HTTP_GET, []() {
    finishWifiWork();
    sendStatic(services::radar_web_pages::kAircraft, "text/html; charset=utf-8");
  });
  server.on("/status", HTTP_GET, []() {
    finishWifiWork();
    sendStatic(services::radar_web_pages::kStatus, "text/html; charset=utf-8");
  });
  server.on("/assets/radar.css", HTTP_GET, []() {
    sendStatic(services::radar_web_pages::kCss, "text/css; charset=utf-8");
  });
  server.on("/assets/general.js", HTTP_GET, []() {
    sendStatic(services::radar_web_pages::kGeneralJs, "application/javascript; charset=utf-8");
  });
  server.on("/assets/aircraft.js", HTTP_GET, []() {
    sendStatic(services::radar_web_pages::kAircraftJs, "application/javascript; charset=utf-8");
  });
  server.on("/assets/status.js", HTTP_GET, []() {
    sendStatic(services::radar_web_pages::kStatusJs, "application/javascript; charset=utf-8");
  });
  // Small readiness probe prevents resending a large watchlist POST while a
  // TLS handshake owns the internal heap. The POST also checks the same gate.
  server.on("/api/save-ready", HTTP_GET, []() {
    const bool ready = s_fetch_save_gate.reserveSaveWindow(millis());
    if (!ready) s_current_http_server->sendHeader("Retry-After", "1");
    sendNoCache(ready ? 200 : 503, "text/plain",
                ready ? "Ready" : "ADS-B download busy", false);
  });
  server.on("/api/general", HTTP_GET, handleGeneralGet);
  server.on("/api/general", HTTP_POST, handleGeneralPost);
  server.on("/api/aircraft", HTTP_GET, handleAircraftGet);
  server.on("/api/aircraft", HTTP_POST, handleAircraftPost);
  server.on("/api/status", HTTP_GET, handleStatusGet);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  Serial.println("WiFi credentials, location, and units cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\r\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\r\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\r\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void ensureWifiManager() {
  services::aircraft_styles::init();
  services::watchlist::init();
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setAPCallback(onConfigPortalApStarted);
  s_wm.setCustomHeadElement(kWifiNavigationHead);
  s_wm.setTitle("Plane Radar Wi-Fi");
  s_wm.setWebServerCallback([]() {
    if (s_wm.server) bindRadarRoutes(*s_wm.server);
  });
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_lan_server || s_wm.getConfigPortalActive()) return;
  const uint32_t now = millis();
  if (s_web_last_start_attempt_ms &&
      static_cast<uint32_t>(now - s_web_last_start_attempt_ms) < 5000u) return;
  s_web_last_start_attempt_ms = now ? now : 1u;

  // Radar HTTP on port 80. WiFiManager opens its separate temporary server
  // on port 81 only when users request Wi-Fi setup from the radar web page.
  s_lan_server.reset(new (std::nothrow) WebServer(80));
  if (!s_lan_server) {
    Serial.println("LAN HTTP allocation failed; will retry");
    return;
  }
  bindRadarRoutes(*s_lan_server);
  s_lan_server->begin();
  s_web_started_ms = millis();
  s_web_last_probe_ms = 0;
  s_web_probe_failures = 0;
  s_web_probe_state = WebProbeState::kIdle;
  s_web_probe_reply.reset();
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) MDNS.addService("http", "tcp", 80);
#endif
  Serial.printf("LAN HTTP startup: standalone port=80, heap=%u\r\n",
                static_cast<unsigned>(ESP.getFreeHeap()));
  Serial.printf("LAN web: http://%s/ (General)\r\n",
                WiFi.localIP().toString().c_str());
  Serial.printf("LAN health check: http://%s/health\r\n",
                WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  // Never retain a self-test socket across listener teardown/recreation.
  s_web_probe_client.stop();
  s_web_probe_state = WebProbeState::kIdle;
  s_web_probe_reply.reset();
  if (!s_lan_server) return;
  s_lan_server->stop();
  s_lan_server.reset();
  s_current_http_server = nullptr;
  s_web_started_ms = 0;
  s_web_last_probe_ms = 0;
  s_web_probe_failures = 0;
#ifdef WM_MDNS
  MDNS.end();
#endif
}

// A small, cooperative HTTP self-test. Like the previous TCP test, it cannot
// prove the *external* route through the user's router, but it does prove
// that handleClient() accepted and answered an actual GET /health request.
// Only main loop accesses this WiFiClient and WebServer: no cross-task calls.
void webProbeFinished(bool healthy, const char* reason) {
  const size_t response_bytes = s_web_probe_reply.length();
  const uint32_t elapsed = millis() - s_web_probe_started_ms;
  s_web_probe_client.stop();
  s_web_probe_state = WebProbeState::kIdle;
  if (healthy) {
    ++s_web_health_ok_total;
    if (s_web_verify_after_rebuild) {
      ++s_web_health_verified_recovery_total;
      s_web_last_verified_recovery_ms = millis();
    }
    if (s_web_probe_failures || s_web_verify_after_rebuild) {
      Serial.printf("LAN HTTP /health verified: after_rebuild=%u "
                    "previous_failures=%u latency=%lums "
                    "ok=%lu failed=%lu rebuilds=%lu\r\n",
                    static_cast<unsigned>(s_web_verify_after_rebuild),
                    static_cast<unsigned>(s_web_probe_failures),
                    static_cast<unsigned long>(elapsed),
                    static_cast<unsigned long>(s_web_health_ok_total),
                    static_cast<unsigned long>(s_web_health_fail_total),
                    static_cast<unsigned long>(s_web_health_rebuild_total));
    }
    s_web_verify_after_rebuild = false;
    s_web_probe_failures = 0;
    return;
  }

  ++s_web_health_fail_total;
  if (s_web_probe_failures < kWebProbeFailuresBeforeRestart)
    ++s_web_probe_failures;
  Serial.printf("LAN HTTP /health failed (%u/%u): reason=%s elapsed=%lums "
                "bytes=%u handler_hits=%lu heap=%u largest=%u wifi=%d rssi=%d "
                "ok=%lu failed=%lu rebuilds=%lu\r\n",
                static_cast<unsigned>(s_web_probe_failures),
                static_cast<unsigned>(kWebProbeFailuresBeforeRestart),
                reason, static_cast<unsigned long>(elapsed),
                static_cast<unsigned>(response_bytes),
                static_cast<unsigned long>(s_web_health_handler_hits -
                                           s_web_probe_handler_hits_at_start),
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                static_cast<int>(WiFi.status()), static_cast<int>(WiFi.RSSI()),
                static_cast<unsigned long>(s_web_health_ok_total),
                static_cast<unsigned long>(s_web_health_fail_total),
                static_cast<unsigned long>(s_web_health_rebuild_total));
  if (s_web_probe_failures < kWebProbeFailuresBeforeRestart) return;
  const uint32_t now = millis();
  if (s_web_last_rebuild_ms &&
      static_cast<uint32_t>(now - s_web_last_rebuild_ms) < kWebRestartCooldownMs) {
    Serial.println("LAN HTTP rebuild deferred: cooldown active");
    return;
  }
  s_web_last_rebuild_ms = now ? now : 1u;
  ++s_web_health_rebuild_total;
  s_web_verify_after_rebuild = true;
  Serial.printf("LAN HTTP /health unavailable: rebuilding standalone server "
                "(rebuild #%lu)\r\n",
                static_cast<unsigned long>(s_web_health_rebuild_total));
  stopLanWebPortal();
  startLanWebPortal();
}

void monitorLanWebPortal() {
  if (!wifiLinkUp() || !s_lan_server || s_wm.getConfigPortalActive() ||
      !s_web_started_ms) return;
  const uint32_t now = millis();

  if (s_web_probe_state == WebProbeState::kAwaitingResponse) {
    // Check after handleClient() has had a chance to respond. Limit both time
    // and bytes per loop so health monitoring cannot stall rendering/HTTP.
    uint16_t processed = 0;
    while (s_web_probe_client.available() > 0 && processed < 192) {
      const int incoming = s_web_probe_client.read();
      if (incoming < 0) break;
      s_web_probe_reply.append(static_cast<uint8_t>(incoming));
      ++processed;
    }
    switch (s_web_probe_reply.result()) {
      case services::web_health::Reply::Result::kOk:
        webProbeFinished(true, "ok");
        return;
      case services::web_health::Reply::Result::kBadStatus:
        webProbeFinished(false, "http_status");
        return;
      case services::web_health::Reply::Result::kBadBody:
        webProbeFinished(false, "bad_health_body");
        return;
      case services::web_health::Reply::Result::kTooLarge:
        webProbeFinished(false, "oversized_response");
        return;
      case services::web_health::Reply::Result::kPending:
        break;
    }
    if (!s_web_probe_client.connected() && s_web_probe_client.available() == 0) {
      webProbeFinished(false, "connection_closed_before_health");
    } else if (static_cast<uint32_t>(now - s_web_probe_started_ms) >=
               kWebProbeResponseTimeoutMs) {
      webProbeFinished(false, "http_timeout");
    }
    return;
  }

  if (static_cast<uint32_t>(now - s_web_started_ms) < kWebStartupGraceMs ||
      (s_web_last_probe_ms &&
       static_cast<uint32_t>(now - s_web_last_probe_ms) < kWebProbeIntervalMs) ||
      s_adsb_fetch_active.load(std::memory_order_relaxed) ||
      radarWorkPausedInternal() || s_wifi_admin_requested) return;

  s_web_last_probe_ms = now;
  s_web_probe_started_ms = now;
  s_web_probe_handler_hits_at_start = s_web_health_handler_hits;
  s_web_probe_reply.reset();
  if (!s_web_probe_client.connect(WiFi.localIP(), 80, 350)) {
    webProbeFinished(false, "tcp_connect");
    return;
  }
  static const char kHealthRequest[] =
      "GET /health HTTP/1.0\r\n"
      "Host: plane-radar-selfcheck\r\n"
      "Connection: close\r\n\r\n";
  const size_t request_length = sizeof(kHealthRequest) - 1;
  const size_t written = s_web_probe_client.write(
      reinterpret_cast<const uint8_t*>(kHealthRequest), request_length);
  if (written != request_length) {
    webProbeFinished(false, "http_request_write");
    return;
  }
  s_web_probe_state = WebProbeState::kAwaitingResponse;
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\r\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }

  if (conf.sta.ssid[0] == '\0') {
    return false;
  }

  // ESP-IDF stores the SSID in a fixed 32-byte field. A maximum-length
  // SSID has no room for a trailing NUL, so copy it to a larger buffer
  // and explicitly terminate it before constructing an Arduino String.
  char ssid_buf[sizeof(conf.sta.ssid) + 1] = {};
  memcpy(ssid_buf, conf.sta.ssid, sizeof(conf.sta.ssid));
  ssid_buf[sizeof(conf.sta.ssid)] = '\0';

  char pass_buf[sizeof(conf.sta.password) + 1] = {};
  memcpy(pass_buf, conf.sta.password, sizeof(conf.sta.password));
  pass_buf[sizeof(conf.sta.password)] = '\0';

  const String ssid(ssid_buf);
  const String pass(pass_buf);

  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  if (s_wm.getWebPortalActive()) s_wm.stopWebPortal();
  s_wifi_admin_requested = false;
  s_wifi_admin_close_requested = false;
  s_wm.setHttpPort(80);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    s_current_http_server = s_wm.server.get();
    const bool connected = s_wm.process();
    s_current_http_server = nullptr;
    if (connected) return true;
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiRadarWorkPaused() { return radarWorkPausedInternal(); }
bool wifiDisplayYieldForWeb() {
  const uint32_t last = s_last_web_served_ms.load(std::memory_order_relaxed);
  return last && static_cast<uint32_t>(millis() - last) < kWebDisplayYieldMs;
}
bool wifiTryBeginAdsbFetch() {
  if (radarWorkPausedInternal() || !s_fetch_save_gate.tryBeginFetch(millis()))
    return false;
  s_adsb_fetch_active.store(true);
  if (radarWorkPausedInternal()) {
    s_adsb_fetch_active.store(false);
    s_fetch_save_gate.endFetch();
    return false;
  }
  return true;
}
void wifiEndAdsbFetch() {
  s_adsb_fetch_active.store(false);
  s_fetch_save_gate.endFetch();
}

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

// 20-second deliberate hold, with a 10-second visible warning.
void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);
    const unsigned long elapsed = millis() - down_ms;
    if (elapsed >= 10000UL && elapsed < config::kBootResetHoldMs) {
      const unsigned long remaining = (config::kBootResetHoldMs - elapsed + 999UL) / 1000UL;
      s_boot_reset_seconds = static_cast<uint8_t>(remaining);
    } else {
      s_boot_reset_seconds = 0;
    }
    if (!s_long_press_handled && elapsed >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      s_boot_reset_seconds = 0;
      Serial.println("BOOT held for 20 seconds - resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
    s_boot_reset_seconds = 0;
  }
}
uint8_t wifiResetCountdownSeconds() { return s_boot_reset_seconds; }

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  return connectSavedNetwork(true);
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    if (!s_lan_server && !s_wm.getConfigPortalActive()) startLanWebPortal();
    if (s_lan_server) {
      s_current_http_server = s_lan_server.get();
      s_lan_server->handleClient();
    }

    // WiFiManager only runs for on-demand Wi-Fi scans and credential changes.
    // It never owns or restarts the standard radar LAN port 80.
    if (s_wifi_admin_requested && !s_wifi_admin_close_requested &&
        !s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      s_wm.setHttpPort(kWifiAdminPort);
      s_wm.setConfigPortalBlocking(false);
      s_wm.startWebPortal();
      Serial.printf("Wi-Fi admin portal on port %u: %s\r\n",
                    static_cast<unsigned>(kWifiAdminPort),
                    s_wm.getWebPortalActive() ? "active" : "failed");
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_current_http_server = s_wm.server.get();
      s_wm.process();
    }
    if (s_wifi_admin_close_requested && s_wm.getWebPortalActive()) {
      s_wm.stopWebPortal();
      s_wifi_admin_requested = false;
      s_wifi_admin_close_requested = false;
      Serial.println("Wi-Fi admin portal closed; radar HTTP remains active");
    }
    s_current_http_server = nullptr;
    monitorLanWebPortal();
  } else {
    stopLanWebPortal();
    if (s_wm.getWebPortalActive()) s_wm.stopWebPortal();
    s_wifi_admin_requested = false;
    s_wifi_admin_close_requested = false;
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\r\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\r\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\r\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\r\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
