#pragma once

#include <cstdint>

namespace services::adsb {

// Small, allocation-free state tracker for ADS-B diagnostics. It does not
// decide whether zero aircraft is an error: an empty but valid array is normal.
class FetchDiagnostics {
 public:
  static constexpr uint32_t kZeroReportIntervalMs = 15u * 60u * 1000u;
  static constexpr uint32_t kInvalidReportIntervalMs = 5u * 60u * 1000u;
  static constexpr uint32_t kZeroRecoveryMinMs = 60u * 1000u;

  enum class EventKind : uint8_t {
    kNone, kZeroStarted, kZeroPeriodic, kZeroEnded,
    kInvalidStarted, kInvalidPeriodic
  };

  struct Event {
    // Explicit constructors allow this value type to compile with both the
    // Arduino ESP32 GCC C++11 settings and the host-side C++17 tests.
    Event() : kind(EventKind::kNone), duration_ms(0), samples(0),
              previous_radius_km(0.0f) {}
    Event(EventKind event_kind, uint32_t duration, uint32_t sample_count,
          float radius_km)
        : kind(event_kind), duration_ms(duration), samples(sample_count),
          previous_radius_km(radius_km) {}

    EventKind kind;
    uint32_t duration_ms;
    uint32_t samples;
    float previous_radius_km;
  };

  Event valid(uint32_t now_ms, unsigned displayed, float radius_km,
              double lat, double lon) {
    invalid_streak_ = 0;
    if (displayed != 0) {
      Event e;
      if (zero_active_) {
        const uint32_t elapsed = now_ms - zero_started_ms_;
        if (elapsed >= kZeroRecoveryMinMs) {
          e = Event(EventKind::kZeroEnded, elapsed, zero_samples_, zero_radius_km_);
        }
      }
      zero_active_ = false;
      return e;
    }

    // A new range or center is a distinct query and starts a new observation.
    if (!zero_active_ || radius_km != zero_radius_km_ ||
        lat != zero_lat_ || lon != zero_lon_) {
      zero_active_ = true;
      zero_started_ms_ = now_ms;
      zero_reported_ms_ = now_ms;
      zero_samples_ = 1;
      zero_radius_km_ = radius_km;
      zero_lat_ = lat;
      zero_lon_ = lon;
      return Event(EventKind::kZeroStarted, 0, zero_samples_, zero_radius_km_);
    }

    ++zero_samples_;
    if (static_cast<uint32_t>(now_ms - zero_reported_ms_) >=
        kZeroReportIntervalMs) {
      zero_reported_ms_ = now_ms;
      return Event(EventKind::kZeroPeriodic, now_ms - zero_started_ms_,
                   zero_samples_, zero_radius_km_);
    }
    return Event();
  }

  // Only for HTTP 200 + complete parse where `ac` is missing/non-array.
  // Does NOT change the last valid aircraft snapshot or reset a zero period.
  Event invalidPayload(uint32_t now_ms) {
    ++invalid_streak_;
    if (invalid_streak_ == 1) {
      last_invalid_report_ms_ = now_ms;
      return Event(EventKind::kInvalidStarted, 0, invalid_streak_, 0.0f);
    }
    if (static_cast<uint32_t>(now_ms - last_invalid_report_ms_) >=
        kInvalidReportIntervalMs) {
      last_invalid_report_ms_ = now_ms;
      return Event(EventKind::kInvalidPeriodic, 0, invalid_streak_, 0.0f);
    }
    return Event();
  }

 private:
  bool zero_active_ = false;
  uint32_t zero_started_ms_ = 0;
  uint32_t zero_reported_ms_ = 0;
  uint32_t zero_samples_ = 0;
  float zero_radius_km_ = 0;
  double zero_lat_ = 0;
  double zero_lon_ = 0;

  uint32_t invalid_streak_ = 0;
  uint32_t last_invalid_report_ms_ = 0;
};

}  // namespace services::adsb
