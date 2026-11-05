#include "ui/turn_prediction.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ui::turn_prediction {
namespace {
constexpr float kPiOver180 = 0.01745329251994329577f;
constexpr float kKmPerDegree = 111.0f;
constexpr float kKmPerKnotSecond = 1.852f / 3600.0f;
constexpr float kStartTurnRate = 0.30f;  // deg/s
constexpr float kStopTurnRate = 0.15f;
constexpr float kMaxObservedRate = 30.0f; // safeguard, not type-specific
constexpr float kFullTurnSeconds = 6.0f;
constexpr float kTurnDecaySeconds = 3.0f;
constexpr float kMaxPositionAgeSeconds = 12.0f;
constexpr uint32_t kCorrectionMs = 1200;
constexpr uint32_t kUnusedStateMs = 120000;

struct State {
  char identity[16];
  uint32_t sample_ms;
  uint32_t last_seen_ms;
  uint32_t last_render_ms;
  uint32_t correction_started_ms;
  float observed_track_deg;
  float observed_nose_deg;
  float track_rate_deg_s;
  float nose_rate_deg_s;
  float displayed_nose_deg;
  float previous_lat;
  float previous_lon;
  float correction_lat;
  float correction_lon;
  bool track_turning;
  bool nose_turning;
  bool initialized;
};

State s_states[services::adsb::kMaxAircraft] = {};

float normalizeHeading(float a) {
  if (!std::isfinite(a)) return 0.0f;
  a = fmodf(a, 360.0f);
  return a < 0.0f ? a + 360.0f : a;
}
float shortestDelta(float a, float b) {
  float d = normalizeHeading(b) - normalizeHeading(a);
  if (d > 180.0f) d -= 360.0f;
  if (d < -180.0f) d += 360.0f;
  return d;
}

// Avoid jitter at straight-flight headings; keep some hysteresis to prevent
// the turn detector flickering at the threshold.
float measuredRate(float previous, float current, float dt_s,
                   float previous_rate, bool* active) {
  if (dt_s < 1.5f || dt_s > 30.0f) {
    *active = false;
    return 0.0f;
  }
  const float raw = std::max(-kMaxObservedRate,
                             std::min(kMaxObservedRate,
                               shortestDelta(previous, current) / dt_s));
  const float magnitude = fabsf(raw);
  if (magnitude <= kStopTurnRate ||
      (!*active && magnitude < kStartTurnRate)) {
    *active = false;
    return 0.0f;
  }
  const bool newly_turning = !*active;
  *active = true;
  if (newly_turning || (previous_rate * raw < 0.0f)) return raw;
  return 0.65f * raw + 0.35f * previous_rate;
}

// Integrated number of seconds of rotation from the last actual fix.
// Keeps turning for 6 s and gradually reduces rate to zero over the next 3 s.
float integratedTurnSeconds(float elapsed) {
  if (elapsed <= 0.0f) return 0.0f;
  const float full = std::min(elapsed, kFullTurnSeconds);
  const float decay = std::max(0.0f,
      std::min(elapsed - kFullTurnSeconds, kTurnDecaySeconds));
  return full + decay - (decay * decay) / (2.0f * kTurnDecaySeconds);
}

State* stateFor(const char* id, uint32_t now) {
  State* free_state = nullptr;
  State* oldest = &s_states[0];
  for (State& s : s_states) {
    if (strcmp(s.identity, id) == 0) return &s;
    if (!free_state && (!s.identity[0] ||
        static_cast<uint32_t>(now - s.last_seen_ms) > kUnusedStateMs))
      free_state = &s;
    if (static_cast<uint32_t>(now - s.last_seen_ms) >
        static_cast<uint32_t>(now - oldest->last_seen_ms)) oldest = &s;
  }
  State* s = free_state ? free_state : oldest;
  *s = {};
  strncpy(s->identity, id, sizeof(s->identity) - 1);
  return s;
}
}  // namespace

void positionAt(const services::adsb::Aircraft& ac, float turn_rate,
                float seconds, float km_per_deg_lon,
                float* out_lat, float* out_lon) {
  *out_lat = ac.lat;
  *out_lon = ac.lon;
  if (seconds <= 0.0f || ac.gs_knots <= 0.0f ||
      !std::isfinite(ac.gs_knots) || fabsf(km_per_deg_lon) < 0.01f) return;
  seconds = std::min(seconds, kMaxPositionAgeSeconds);
  const float speed = ac.gs_knots * kKmPerKnotSecond;
  const float heading_rad = normalizeHeading(ac.track_deg) * kPiOver180;
  const float rate_rad = turn_rate * kPiOver180;
  float east = 0.0f, north = 0.0f;
  if (fabsf(turn_rate) < kStopTurnRate) {
    east = speed * seconds * sinf(heading_rad);
    north = speed * seconds * cosf(heading_rad);
  } else {
    // Exact circular arc during the first 6 seconds.
    const float arc_seconds = std::min(seconds, kFullTurnSeconds);
    const float end_heading = heading_rad + rate_rad * arc_seconds;
    east = speed * (cosf(heading_rad) - cosf(end_heading)) / rate_rad;
    north = speed * (sinf(end_heading) - sinf(heading_rad)) / rate_rad;

    // For stale fixes, gradually reduce turn-rate. Two midpoint segments
    // approximate the 3 s taper; they cost nothing during normal (<6s) data.
    const float taper_seconds = std::min(
        std::max(seconds - kFullTurnSeconds, 0.0f), kTurnDecaySeconds);
    if (taper_seconds > 0.0f) {
      for (int i = 0; i < 2; ++i) {
        const float mid = (static_cast<float>(i) + 0.5f) * taper_seconds / 2.0f;
        const float integral = kFullTurnSeconds + mid -
           (mid * mid) / (2.0f * kTurnDecaySeconds);
        const float angle = heading_rad + rate_rad * integral;
        const float distance = speed * taper_seconds * 0.5f;
        east += distance * sinf(angle);
        north += distance * cosf(angle);
      }
    }
    const float extra_straight = std::max(
        0.0f, seconds - kFullTurnSeconds - kTurnDecaySeconds);
    if (extra_straight > 0.0f) {
      const float angle = heading_rad + rate_rad *
        (kFullTurnSeconds + kTurnDecaySeconds * 0.5f);
      east += speed * extra_straight * sinf(angle);
      north += speed * extra_straight * cosf(angle);
    }
  }
  *out_lat += north / kKmPerDegree;
  *out_lon += east / km_per_deg_lon;
}

Pose predict(const char* identity, const services::adsb::Aircraft& ac,
             uint32_t fetch_ms, uint32_t now_ms, float km_per_deg_lon) {
  Pose result;
  result.lat = ac.lat;
  result.lon = ac.lon;
  result.nose_deg = normalizeHeading(ac.nose_deg);
  result.track_deg = normalizeHeading(ac.track_deg);
  if (!identity || !identity[0] || !fetch_ms) return result;

  State* s = stateFor(identity, now_ms);
  const uint32_t sample_ms = fetch_ms - ac.pos_age_ms;
  const bool first = !s->initialized;
  const bool changed = !first && sample_ms != s->sample_ms &&
                       static_cast<int32_t>(sample_ms - s->sample_ms) > 0;
  if (first) {
    s->initialized = true;
    s->sample_ms = sample_ms;
    s->observed_track_deg = result.track_deg;
    s->observed_nose_deg = result.nose_deg;
    s->displayed_nose_deg = result.nose_deg;
    s->last_render_ms = now_ms;
  } else if (changed) {
    const float dt_s = static_cast<float>(sample_ms - s->sample_ms) * 0.001f;
    s->track_rate_deg_s = measuredRate(s->observed_track_deg,
      result.track_deg, dt_s, s->track_rate_deg_s, &s->track_turning);
    s->nose_rate_deg_s = measuredRate(s->observed_nose_deg,
      result.nose_deg, dt_s, s->nose_rate_deg_s, &s->nose_turning);
    s->observed_track_deg = result.track_deg;
    s->observed_nose_deg = result.nose_deg;
    s->sample_ms = sample_ms;
  }

  const float age_seconds = std::min(kMaxPositionAgeSeconds,
      static_cast<float>(now_ms - sample_ms) * 0.001f);
  result.age_s = age_seconds;
  result.track_rate_deg_s = s->track_rate_deg_s;
  result.turning = s->track_turning && fabsf(s->track_rate_deg_s) >= kStopTurnRate;
  positionAt(ac, s->track_rate_deg_s, age_seconds, km_per_deg_lon,
             &result.lat, &result.lon);

  // Reconcile a newly received real fix without snapping the symbol. The
  // display-only positional correction fades out fully in 1.2 seconds.
  if (changed) {
    const float difference_lat = s->previous_lat - result.lat;
    const float difference_lon = s->previous_lon - result.lon;
    const float east = difference_lon * km_per_deg_lon;
    const float north = difference_lat * kKmPerDegree;
    const float error_km_sq = east * east + north * north;
    if (error_km_sq < 4.0f) {
      s->correction_lat = difference_lat;
      s->correction_lon = difference_lon;
      s->correction_started_ms = now_ms;
    } else {
      s->correction_lat = 0.0f;
      s->correction_lon = 0.0f;
    }
  }
  const uint32_t correction_elapsed = now_ms - s->correction_started_ms;
  if (correction_elapsed < kCorrectionMs) {
    const float remaining = 1.0f -
      static_cast<float>(correction_elapsed) / kCorrectionMs;
    result.lat += s->correction_lat * remaining * remaining;
    result.lon += s->correction_lon * remaining * remaining;
  }

  const float predicted_nose = normalizeHeading(ac.nose_deg +
    s->nose_rate_deg_s * integratedTurnSeconds(age_seconds));
  if (first) {
    s->displayed_nose_deg = predicted_nose;
  } else {
    const float dt = std::min(1.5f,
       static_cast<float>(now_ms - s->last_render_ms) * 0.001f);
    // 450 ms smoothing constant; allows fast turns yet avoids update jumps.
    const float fraction = dt / (0.45f + dt);
    s->displayed_nose_deg = normalizeHeading(s->displayed_nose_deg +
      shortestDelta(s->displayed_nose_deg, predicted_nose) * fraction);
  }
  result.nose_deg = s->displayed_nose_deg;
  result.track_deg = normalizeHeading(ac.track_deg +
    s->track_rate_deg_s * integratedTurnSeconds(age_seconds));
  s->previous_lat = result.lat;
  s->previous_lon = result.lon;
  s->last_seen_ms = now_ms;
  s->last_render_ms = now_ms;
  return result;
}

void reset() { for (State& s : s_states) s = {}; }

}  // namespace ui::turn_prediction
