#include "services/adsb_client.h"

#include <WiFi.h>
#include <ESP_SSLClient.h>

#include <ArduinoJson.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_heap_caps.h>
#include <esp_err.h>

#include <cstring>

#include "config.h"
#include "services/http_body_framing.h"
#include "services/http_response_head.h"

namespace services::adsb {

namespace {

constexpr char kApiHost[] = "opendata.adsb.fi";
constexpr char kApiPath[] = "/api/v3/lat/";
// Full-size TLS RX accommodates maximum unnegotiated server TLS records.
// TX only carries small HTTP/1.1 GET requests; BearSSL handles fragmentation.
constexpr int kBearSslReceiveBufferBytes = 16709;
constexpr int kBearSslTransmitBufferBytes = 1024;
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectTimeoutMs = 5000;  // TLS handshake needs room
constexpr unsigned long kRequestTimeoutMs = 6000;


// Failed allocations are captured without logging or allocating from
// the failure callback. The ESP32-C3 has one CPU core; these volatile fields
// are intentionally best-effort diagnostics, not an allocation-event journal.
enum class FetchPhase : uint8_t { kIdle, kHttpGet, kParsing };
volatile uint32_t s_alloc_fail_count = 0;
volatile uint32_t s_alloc_fail_bytes = 0;
volatile uint32_t s_alloc_fail_caps = 0;
const char* volatile s_alloc_fail_func = nullptr;
volatile FetchPhase s_fetch_phase = FetchPhase::kIdle;
volatile FetchPhase s_last_fail_phase = FetchPhase::kIdle;

void onHeapAllocationFailed(size_t requested, uint32_t caps,
                            const char* function_name) {
  // No Serial, heap APIs, locks, or dynamic allocation in this hook.
  s_alloc_fail_bytes = static_cast<uint32_t>(requested);
  s_alloc_fail_caps = caps;
  s_alloc_fail_func = function_name;  // IDF's static function-name string.
  s_last_fail_phase = s_fetch_phase;
  ++s_alloc_fail_count;  // Publish event after recording its details.
}

struct HeapSample {
  uint32_t free_bytes = 0;
  uint32_t largest_bytes = 0;
};

HeapSample heapSample() {
  HeapSample sample;
  constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  sample.free_bytes = static_cast<uint32_t>(heap_caps_get_free_size(caps));
  sample.largest_bytes =
      static_cast<uint32_t>(heap_caps_get_largest_free_block(caps));
  return sample;
}

const char* phaseName(FetchPhase phase) {
  switch (phase) {
    case FetchPhase::kHttpGet: return "GET";
    case FetchPhase::kParsing: return "parse";
    default: return "other";
  }
}

// Lifetime intentionally encloses URL, HTTP client, TLS client, JSON documents
// and socket buffers. Its destructor samples memory AFTER they are destroyed.
class FetchTrace {
 public:
  FetchTrace() : initial_fail_count_(s_alloc_fail_count) { ++s_attempts_; }

  ~FetchTrace() {
    s_fetch_phase = FetchPhase::kIdle;
    if (success_) {
      ++s_successes_;
      s_consecutive_failures_ = 0;
    } else {
      ++s_failures_;
      ++s_consecutive_failures_;
    }
    const uint32_t now = millis();
    const uint32_t new_alloc_fails = s_alloc_fail_count - initial_fail_count_;
    const bool anomaly = !success_ || new_alloc_fails != 0;
    // Keep useful failure evidence, but don't flood the serial monitor with
    // the experimental five-stage heap report every 30 seconds.
    if (!anomaly && s_successes_ > 1 &&
        static_cast<uint32_t>(now - s_last_log_ms_) < 300000u)
      return;
    s_last_log_ms_ = now;
    const unsigned int stack_free_min =
        static_cast<unsigned int>(uxTaskGetStackHighWaterMark(nullptr));
    Serial.printf(
        "tls diag %s: attempts=%lu ok=%lu failed=%lu consecutive=%lu "
        "http=%d stack_min_free=%uB heap_min=%uB\r\n",
        success_ ? "ok" : "failed",
        static_cast<unsigned long>(s_attempts_),
        static_cast<unsigned long>(s_successes_),
        static_cast<unsigned long>(s_failures_),
        static_cast<unsigned long>(s_consecutive_failures_), http_code_,
        stack_free_min, static_cast<unsigned>(ESP.getMinFreeHeap()));
    if (anomaly) {
      const HeapSample finished = heapSample();
      Serial.printf(
          "tls heap internal8 free/largest: pre_GET=%lu/%lu "
          "after_GET=%lu/%lu after_parse=%lu/%lu "
          "after_http_end=%lu/%lu final=%lu/%lu\r\n",
          static_cast<unsigned long>(before_get_.free_bytes),
          static_cast<unsigned long>(before_get_.largest_bytes),
          static_cast<unsigned long>(after_get_.free_bytes),
          static_cast<unsigned long>(after_get_.largest_bytes),
          static_cast<unsigned long>(after_parse_.free_bytes),
          static_cast<unsigned long>(after_parse_.largest_bytes),
          static_cast<unsigned long>(after_end_.free_bytes),
          static_cast<unsigned long>(after_end_.largest_bytes),
          static_cast<unsigned long>(finished.free_bytes),
          static_cast<unsigned long>(finished.largest_bytes));
    }
    if (new_alloc_fails != 0) {
      Serial.printf(
          "tls alloc failures during fetch=%lu (global=%lu), "
          "last requested=%luB caps=0x%08lX caller=%s phase=%s\r\n",
          static_cast<unsigned long>(new_alloc_fails),
          static_cast<unsigned long>(s_alloc_fail_count),
          static_cast<unsigned long>(s_alloc_fail_bytes),
          static_cast<unsigned long>(s_alloc_fail_caps),
          s_alloc_fail_func != nullptr ? s_alloc_fail_func : "unknown",
          phaseName(s_last_fail_phase));
    }
  }

  void beforeGet() {
    before_get_ = heapSample();
    s_fetch_phase = FetchPhase::kHttpGet;
  }
  void afterGet(int http_code) {
    s_fetch_phase = FetchPhase::kIdle;
    http_code_ = http_code;
    after_get_ = heapSample();
  }
  void beforeParse() { s_fetch_phase = FetchPhase::kParsing; }
  void afterParse() {
    s_fetch_phase = FetchPhase::kIdle;
    after_parse_ = heapSample();
  }
  void afterHttpEnd() { after_end_ = heapSample(); }
  void markSuccess() { success_ = true; }

 private:
  HeapSample before_get_;
  HeapSample after_get_;
  HeapSample after_parse_;
  HeapSample after_end_;
  const uint32_t initial_fail_count_;
  int http_code_ = 0;
  bool success_ = false;
  static uint32_t s_attempts_;
  static uint32_t s_successes_;
  static uint32_t s_failures_;
  static uint32_t s_consecutive_failures_;
  static uint32_t s_last_log_ms_;
};
uint32_t FetchTrace::s_attempts_ = 0;
uint32_t FetchTrace::s_successes_ = 0;
uint32_t FetchTrace::s_failures_ = 0;
uint32_t FetchTrace::s_consecutive_failures_ = 0;
uint32_t FetchTrace::s_last_log_ms_ = 0;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
unsigned long s_last_update_ms = 0;
PollFn s_poll_fn = nullptr;
SemaphoreHandle_t s_mutex = nullptr;

/** Publish parsed aircraft to the shared buffer atomically. */
void publish(const Aircraft* src, size_t count) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  for (size_t i = 0; i < count; ++i) {
    s_aircraft[i] = src[i];
  }
  s_aircraft_count = count;
  s_last_update_ms = millis();  // base time for dead-reckoning
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

// For this controlled experiment we use one fresh BearSSL connection per
// request. Keep-alive/connection reuse stays OFF to isolate the TLS change.

/**
 * Pumps the socket one byte at a time, in blocks, without buffering the whole
 * response.
 *
 * Each refill runs the network poll callback. The wire image is raw HTTP/1.1
 * (including chunked transfer framing); BodyFramer unwraps it as before.
 *
 * Where the body ends is BodyFramer's business, not this class's: it stops
 * pulling at the right byte. Reading a little past that point into buffer_ is
 * harmless while the connection is torn down after every fetch.
 */
class PollingSocketSource {
 public:
  PollingSocketSource(Client& stream, unsigned long deadline)
      : stream_(&stream), deadline_(deadline) {}

  /** Next raw byte, or -1 once the socket closes or the deadline passes. */
  int read() {
    if (pos_ >= len_ && !refill()) {
      return -1;
    }
    return static_cast<unsigned char>(buffer_[pos_++]);
  }

 private:
  bool refill() {
    pos_ = 0;
    len_ = 0;
    while (millis() < deadline_) {
      pollNetwork();
      const int available = stream_->available();
      if (available > 0) {
        const int to_read = available > static_cast<int>(sizeof(buffer_))
                                ? static_cast<int>(sizeof(buffer_))
                                : available;
        const int read_bytes = stream_->readBytes(buffer_, to_read);
        if (read_bytes > 0) {
          len_ = static_cast<size_t>(read_bytes);
          return true;
        }
      }
      if (!stream_->connected() && stream_->available() <= 0) {
        break;  // server closed and the socket is drained
      }
      delay(1);
    }
    return false;
  }

  Client* stream_;
  unsigned long deadline_;
  char buffer_[512];
  size_t pos_ = 0;
  size_t len_ = 0;
};

// No response body is buffered in RAM. Keep HTTP/1.1 response parsing bounded
// and preserve the original incremental JSON parser.
bool sendBearSslGet(ESP_SSLClient& client, const char* path) {
  char request[256];
  const int n = snprintf(request, sizeof(request),
      "GET %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "Accept: application/json\r\n"
      "Accept-Encoding: identity\r\n"
      "Connection: close\r\n"
      "User-Agent: PlaneRadar-v2.0.0\r\n\r\n",
      path, kApiHost);
  if (n <= 0 || n >= static_cast<int>(sizeof(request))) return false;
  return client.write(reinterpret_cast<const uint8_t*>(request),
                      static_cast<size_t>(n)) == static_cast<size_t>(n);
}

using BodyReader = services::http::BodyFramer<PollingSocketSource>;

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

bool isMilitary(const JsonObject& plane) {
  // dbFlags bit 0 = military, absent/unknown -> civilian colour.
  return (plane["dbFlags"].as<unsigned int>() & 1U) != 0;
}
float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

// Rotorcraft category A7 first, then common ICAO helicopter types.
bool isHelicopter(const JsonObject& obj) {
  const char* category = obj["category"].as<const char*>();
  if (category && strcmp(category,"A7") == 0) return true;
  const char* type = obj["t"].as<const char*>();
  if (!type) return false;
  constexpr const char* types[] = {"R22","R44","R66","B06","B407","B429",
      "H125","H130","H135","H145","H155","EC20","EC35","EC45","EC55",
      "AS50","AS55","AS65","A109","A119","A139","A169","A189","S76",
      "S92","UH60","CH47","AH64","MI8","MI17","MI24"};
  for (const char* known : types) if (strcmp(known,type)==0) return true;
  return false;
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "hex", ac->hex, sizeof(ac->hex));
  copyJsonStringTrimmed(plane, "r", ac->registration, sizeof(ac->registration));
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

}  // namespace

void init() {
  Serial.printf("ADSB TLS backend: BearSSL (RX=%dB TX=%dB, v2.0.0)\r\n",
                kBearSslReceiveBufferBytes, kBearSslTransmitBufferBytes);
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
  const esp_err_t result =
      heap_caps_register_failed_alloc_callback(onHeapAllocationFailed);
  Serial.printf("tls allocation hook: %s (0x%X)\r\n",
                result == ESP_OK ? "enabled" : "unavailable",
                static_cast<unsigned>(result));
}

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

unsigned long lastUpdateMs() { return s_last_update_ms; }

size_t snapshotAircraft(Aircraft* out, size_t max_out,
                        unsigned long* out_last_update_ms) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  const size_t count =
      s_aircraft_count < max_out ? s_aircraft_count : max_out;
  for (size_t i = 0; i < count; ++i) {
    out[i] = s_aircraft[i];
  }
  if (out_last_update_ms != nullptr) {
    *out_last_update_ms = s_last_update_ms;
  }
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
  return count;
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  FetchTrace trace;  // Destruct last, after TLS/JSON objects release heap.
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  // Build an independent URL path for the existing coordinate/range API.
  char path[104];
  const int path_len = snprintf(path, sizeof(path), "%s%.6f/lon/%.6f/dist/%.1f",
      kApiPath, center_lat, center_lon, dist_nm);
  if (path_len <= 0 || path_len >= static_cast<int>(sizeof(path))) {
    Serial.println("adsb BearSSL: URL path too long");
    return false;
  }

  // Fresh BearSSL handshake each poll. The underlay must outlive the TLS
  // client: declared first, destroyed after client.stop().
  WiFiClient plain_client;
  ESP_SSLClient client;
  client.setClient(&plain_client);
  client.setInsecure();  // Same certificate policy as frozen V17.32.
  client.setBufferSizes(kBearSslReceiveBufferBytes,
                        kBearSslTransmitBufferBytes);
  client.setHandshakeTimeout(5);  // Library API uses seconds.
  client.setDebugLevel(0);

  // Keep the existing five-stage heap measurements and allocation callback.
  pollNetwork();
  trace.beforeGet();
  const bool connected = client.connect(kApiHost, 443, kConnectTimeoutMs) != 0;
  const bool requested = connected && sendBearSslGet(client, path);
  PollingSocketSource source(client, millis() + kRequestTimeoutMs);
  services::http::ResponseHead response;
  const bool head_ok = requested &&
      services::http::readResponseHead(source, &response);
  const int code = head_ok ? response.status : -1;
  trace.afterGet(code);

  if (code != 200) {
    Serial.printf("adsb BearSSL: HTTP %d (%s)\r\n", code,
        !connected ? "TLS connection failed" :
        !requested ? "request write failed" :
        !head_ok ? "response header failed" : "server rejected request");
    client.stop();
    trace.afterHttpEnd();
    return false;
  }

  const services::http::BodyFraming framing = response.chunked
      ? services::http::BodyFraming::kChunked
      : services::http::BodyFraming::kIdentity;
  const int content_length = response.content_length;

  // Keep only the fields we render; the rest never reaches RAM.
  JsonDocument filter;
  JsonObject f = filter["ac"].add<JsonObject>();
  for (const char* key :
       {"lat", "lon", "true_heading", "mag_heading", "track", "dir", "gs",
        "tas", "ias", "alt_baro", "alt_geom", "seen_pos", "flight", "hex", "t",
        "category", "dbFlags", "r"}) {
    f[key] = true;
  }

  BodyReader body(source, framing, content_length);
  JsonDocument doc;
  trace.beforeParse();
  const DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  trace.afterParse();
  // Read off the terminating chunk the parser stopped short of, so the socket
  // sits at the end of the message. Not needed while every fetch builds its own
  // connection, but a prerequisite for ever reusing one.
  body.drain();
  const bool close_delimited = !response.chunked && content_length < 0;
  // BodyFramer deliberately does not drain close-delimited bodies (there is
  // no next response to preserve). For those, a complete JSON parse is enough;
  // for chunked/length-framed replies require the entire wire body.
  const bool complete_body = !body.framingError() && !body.truncated() &&
      (body.complete() || (close_delimited && !err && body.bytesRead() > 0));
  client.stop();
  trace.afterHttpEnd();
  if (err || !complete_body) {
    if (body.framingError() || body.truncated()) {
      Serial.println("adsb: malformed chunked body");
    } else if (!complete_body) {
      Serial.println("adsb BearSSL: incomplete HTTP body");
    } else if (body.bytesRead() == 0) {
      Serial.println("adsb: empty response");
    } else {
      Serial.printf("adsb: JSON parse error: %s\r\n", err.c_str());
    }
    return false;
  }

  // Parse into a local buffer, then publish atomically so a reader on another
  // thread never sees a half-updated list.
  Aircraft parsed[kMaxAircraft];
  size_t n = 0;
  JsonArray ac = doc["ac"].as<JsonArray>();
  if (!ac.isNull()) {
    for (JsonObject plane : ac) {
      if (n >= kMaxAircraft) {
        break;
      }
      if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
        continue;
      }
      if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
        continue;
      }

      parsed[n].lat = plane["lat"].as<float>();
      parsed[n].lon = plane["lon"].as<float>();
      parsed[n].nose_deg = pickNoseHeading(plane);
      parsed[n].track_deg = pickTrackHeading(plane);
      parsed[n].gs_knots = pickGroundSpeed(plane);
      parsed[n].isMilitary = isMilitary(plane);

      // seen_pos: seconds since this position was measured. Use it as the
      // dead-reckoning age offset, capped so a very stale fix isn't flung far.
      float seen_pos = 0.0f;
      readJsonFloat(plane, "seen_pos", &seen_pos);
      if (seen_pos < 0.0f) seen_pos = 0.0f;
      if (seen_pos > 30.0f) seen_pos = 30.0f;
      parsed[n].pos_age_ms = static_cast<uint32_t>(seen_pos * 1000.0f);

      fillTagFields(&parsed[n], plane);
      parsed[n].isHelicopter = isHelicopter(plane);
      ++n;
    }
  }

  publish(parsed, n);
  Serial.printf("adsb: %u aircraft\r\n", static_cast<unsigned>(n));
  trace.markSuccess();
  static uint32_t successful_fetches = 0;
  if (++successful_fetches % 100 == 0) {
    const HeapSample after_fetch = heapSample();
    Serial.printf("BearSSL v2.0.0: successful=%lu RX=%d TX=%d "
                  "free=%lu largest=%lu min_free=%lu\r\n",
                  static_cast<unsigned long>(successful_fetches),
                  kBearSslReceiveBufferBytes, kBearSslTransmitBufferBytes,
                  static_cast<unsigned long>(after_fetch.free_bytes),
                  static_cast<unsigned long>(after_fetch.largest_bytes),
                  static_cast<unsigned long>(ESP.getMinFreeHeap()));
  }
  return true;
}

}  // namespace services::adsb
