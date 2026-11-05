#include "ui/runway_overlay.h"
#include <algorithm>
#include "ui/pixel_label_font.h"


#include <cmath>
#include <cstdlib>
#include <cstring>

#include "data/large_airports.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::runway {
namespace {

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;
constexpr size_t kMaxAirportLabels = 32;

// 0 = unchecked, 1 = outside search radius, 2 = within search radius,
// 3 = within radius and already scheduled for an airport label.
// Uses a compact combined airport-state cache.
uint8_t s_airport_state[data::large_airports::kAirportCount];

// Visible runways rarely approach this count on a 240x240 local radar.
// A bounded cache avoids heap allocation and falls back to the original
// full scan if more runways become visible; no runways are ever omitted.
constexpr size_t kMaxCachedRunways = 96;
struct CachedRunway {
  int16_t x0, y0, x1, y1;
};
static CachedRunway s_cached_runways[kMaxCachedRunways];
static uint16_t s_cached_airport_labels[kMaxAirportLabels];
struct FixedAirportLabel {
  uint16_t airport;
  int16_t x, y;  // top-left glyph position, computed at range/location change
};
static FixedAirportLabel s_fixed_labels[kMaxAirportLabels];
static size_t s_fixed_label_count = 0;
static size_t s_cached_runway_count = 0;
static size_t s_cached_label_count = 0;
static bool s_cache_valid = false;
static bool s_cache_overflow = false;
static double s_cache_lat = 0.0;
static double s_cache_lon = 0.0;
static float s_cache_range = 0.0f;


float e7ToDeg(int32_t e7) { return static_cast<float>(e7) * 1e-7f; }

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  // Longitude degrees shrink toward the poles; scale by cos(latitude) so
  // east-west distance isn't overstated away from the equator.
  const float center_lat_rad =
      static_cast<float>(services::location::lat()) * kDegToRad;
  *dx_km = static_cast<float>(lon - services::location::lon()) * kKmPerDeg *
           cosf(center_lat_rad);
  *dy_km =
      static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km =
      static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = radar::kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

bool segmentIntersectsDisc(int x0, int y0, int x1, int y1) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int r_sq = r * r;

  if (distSqFromCenter(x0, y0) <= r_sq || distSqFromCenter(x1, y1) <= r_sq) {
    return true;
  }

  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int fx = x0 - cx;
  const int fy = y0 - cy;
  const int a = dx * dx + dy * dy;
  if (a == 0) {
    return false;
  }
  const int b = 2 * (fx * dx + fy * dy);
  const int c = fx * fx + fy * fy - r_sq;
  int disc = b * b - 4 * a * c;
  if (disc < 0) {
    return false;
  }
  disc = static_cast<int>(sqrtf(static_cast<float>(disc)));
  const float inv2a = 1.0f / (2.0f * static_cast<float>(a));
  const float t0 = (-static_cast<float>(b) - disc) * inv2a;
  const float t1 = (-static_cast<float>(b) + disc) * inv2a;
  return (t0 >= 0.0f && t0 <= 1.0f) || (t1 >= 0.0f && t1 <= 1.0f);
}

bool projectRunwayLine(const data::large_airports::Runway& rw,
                       CachedRunway* out) {
  const float le_lat = e7ToDeg(rw.le_lat_e7);
  const float le_lon = e7ToDeg(rw.le_lon_e7);
  const float he_lat = e7ToDeg(rw.he_lat_e7);
  const float he_lon = e7ToDeg(rw.he_lon_e7);

  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  latLonToScreen(le_lat, le_lon, &x0, &y0);
  latLonToScreen(he_lat, he_lon, &x1, &y1);
  if (!segmentIntersectsDisc(x0, y0, x1, y1)) return false;
  clipPointToOuterRing(x0, y0, &x1, &y1);
  clipPointToOuterRing(x1, y1, &x0, &y0);
  *out = {static_cast<int16_t>(x0), static_cast<int16_t>(y0),
          static_cast<int16_t>(x1), static_cast<int16_t>(y1)};
  return true;
}

void drawProjectedRunway(lgfx::LGFXBase& gfx, const CachedRunway& line) {
  gfx.drawWideLine(line.x0, line.y0, line.x1, line.y1,
                   radar::kRunwayLineHalfWidth, radar::kColorRunway);
}

// The label solution is calculated once per location/range, never per frame.
void positionAirportLabels();

void rebuildRunwayCache(double center_lat, double center_lon, float radius_km) {
  s_cache_lat = center_lat;
  s_cache_lon = center_lon;
  s_cache_range = radius_km;
  s_cached_runway_count = 0;
  s_cached_label_count = 0;
  s_fixed_label_count = 0;
  s_cache_overflow = false;
  s_cache_valid = true;

  const double center_cos = fabs(cos(center_lat * kDegToRad));
  const double safe_cos = center_cos > 0.0001 ? center_cos : 0.0001;
  const int64_t center_lat_e7 = static_cast<int64_t>(llround(center_lat * 1e7));
  const int64_t center_lon_e7 = static_cast<int64_t>(llround(center_lon * 1e7));
  const int64_t lat_limit_e7 = static_cast<int64_t>(ceil((radius_km / kKmPerDeg + 0.001) * 1e7));
  const int64_t lon_limit_e7 = static_cast<int64_t>(ceil((radius_km / (kKmPerDeg * safe_cos) + 0.001) * 1e7));

  memset(s_airport_state, 0, sizeof(s_airport_state));
  for (size_t i = 0; i < data::large_airports::kRunwayCount; ++i) {
    const auto& rw = data::large_airports::kRunways[i];
    const uint16_t ap_idx = rw.airport_idx;
    uint8_t& ap_state = s_airport_state[ap_idx];
    if (ap_state == 0) {
      const auto& ap = data::large_airports::kAirports[ap_idx];
      const int64_t dlat = static_cast<int64_t>(ap.lat_e7) - center_lat_e7;
      int64_t dlon = static_cast<int64_t>(ap.lon_e7) - center_lon_e7;
      if (dlon > 1800000000LL) dlon -= 3600000000LL;
      if (dlon < -1800000000LL) dlon += 3600000000LL;
      if (llabs(dlat) > lat_limit_e7 || llabs(dlon) > lon_limit_e7) {
        ap_state = 1;
      } else {
        float dx_km = 0.0f, dy_km = 0.0f, dist_km = 0.0f;
        offsetKmFromCenter(e7ToDeg(ap.lat_e7), e7ToDeg(ap.lon_e7),
                           &dx_km, &dy_km, &dist_km);
        ap_state = dist_km <= radius_km ? 2 : 1;
      }
    }
    if (ap_state == 1) continue;
    CachedRunway line = {};
    if (!projectRunwayLine(rw, &line)) continue;
    if (s_cached_runway_count >= kMaxCachedRunways) {
      // Overflow only affects runway geometry caching. Continue collecting
      // airport identifiers so their label positions can still be cached.
      s_cache_overflow = true;
    } else {
      s_cached_runways[s_cached_runway_count++] = line;
    }
    if (ap_state == 2 && s_cached_label_count < kMaxAirportLabels) {
      ap_state = 3;
      s_cached_airport_labels[s_cached_label_count++] = ap_idx;
    }
  }
  positionAirportLabels();
}

void clipPointOntoOuterRing(int* x, int* y) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int dx = *x - cx;
  const int dy = *y - cy;
  const int d_sq = dx * dx + dy * dy;
  const int r_sq = r * r;
  if (d_sq <= r_sq || d_sq == 0) {
    return;
  }
  const float scale = static_cast<float>(r) / sqrtf(static_cast<float>(d_sq));
  *x = cx + static_cast<int>(lroundf(static_cast<float>(dx) * scale));
  *y = cy + static_cast<int>(lroundf(static_cast<float>(dy) * scale));
}

struct AirportLabelRect { int x0, y0, x1, y1; };

bool airportRectFitsDisc(const AirportLabelRect& r) {
  if (r.x0 < 1 || r.y0 < 1 || r.x1 >= radar::kSize-1 ||
      r.y1 >= radar::kSize-1) return false;
  constexpr int radius_sq = 115 * 115;
  for (int x : {r.x0, r.x1}) for (int y : {r.y0, r.y1}) {
    const int dx = x - radar::kCenterX, dy = y - radar::kCenterY;
    if (dx*dx + dy*dy > radius_sq) return false;
  }
  return true;
}

bool airportRectsOverlap(const AirportLabelRect& a,
                         const AirportLabelRect& b, int margin) {
  return a.x0 < b.x1 + margin && b.x0 < a.x1 + margin &&
         a.y0 < b.y1 + margin && b.y0 < a.y1 + margin;
}

// Liang-Barsky segment/rectangle test, with a 3 px line-clearance margin.
// Used only while recalculating a new range/location, not at 4 Hz.
bool runwayTouchesAirportLabel(const CachedRunway& l,
                               const AirportLabelRect& rect) {
  const float x0 = static_cast<float>(l.x0);
  const float y0 = static_cast<float>(l.y0);
  const float dx = static_cast<float>(l.x1 - l.x0);
  const float dy = static_cast<float>(l.y1 - l.y0);
  const float left = static_cast<float>(rect.x0 - 3);
  const float right = static_cast<float>(rect.x1 + 3);
  const float top = static_cast<float>(rect.y0 - 3);
  const float bottom = static_cast<float>(rect.y1 + 3);
  float t0 = 0.0f, t1 = 1.0f;
  const float ps[4] = {-dx, dx, -dy, dy};
  const float qs[4] = {x0-left, right-x0, y0-top, bottom-y0};
  for (int i=0; i<4; ++i) {
    if (fabsf(ps[i]) < 0.00001f) {
      if (qs[i] < 0.0f) return false;
      continue;
    }
    const float t = qs[i]/ps[i];
    if (ps[i] < 0.0f) t0 = std::max(t0,t);
    else t1 = std::min(t1,t);
    if (t0 > t1) return false;
  }
  return true;
}

bool airportLabelOverlapsRunway(const AirportLabelRect& r) {
  for (size_t i=0; i<s_cached_runway_count; ++i) {
    if (runwayTouchesAirportLabel(s_cached_runways[i],r)) return true;
  }
  if (!s_cache_overflow) return false;
  // Dense-area fallback. Recheck remaining runways only while recalculating
  // label placement so even the overflow case never covers a landing strip.
  size_t visible = 0;
  for (size_t i=0; i<data::large_airports::kRunwayCount; ++i) {
    CachedRunway line = {};
    if (!projectRunwayLine(data::large_airports::kRunways[i], &line)) continue;
    if (visible++ < kMaxCachedRunways) continue;
    if (runwayTouchesAirportLabel(line,r)) return true;
  }
  return false;
}

void positionAirportLabels() {
  s_fixed_label_count = 0;
  for (size_t i=0; i<s_cached_label_count; ++i) {
    const uint16_t ap_idx = s_cached_airport_labels[i];
    const auto& ap = data::large_airports::kAirports[ap_idx];
    const int width = plane_label_font::width(ap.ident, false);
    const int height = 9;
    if (width <= 0 || width > 110) continue;
    int ax=0,ay=0;
    latLonToScreen(e7ToDeg(ap.lat_e7),e7ToDeg(ap.lon_e7),&ax,&ay);
    clipPointOntoOuterRing(&ax,&ay);
    bool placed = false;
    for (int gap : {8, 15, 23}) {
      // Nearby first; expand only when the airport's runways block the text.
      const int x_candidates[8] = {ax-width/2, ax-width/2, ax+gap,
          ax-width-gap, ax+gap, ax-width-gap, ax+gap, ax-width-gap};
      const int y_candidates[8] = {ay-height-gap, ay+gap, ay-height/2,
          ay-height/2, ay-height-gap, ay-height-gap, ay+gap, ay+gap};
      for (int candidate=0; candidate<8; ++candidate) {
        AirportLabelRect rect = {x_candidates[candidate],y_candidates[candidate],
            x_candidates[candidate]+width,y_candidates[candidate]+height};
        if (!airportRectFitsDisc(rect)) continue;
        bool overlaps = false;
        for (size_t existing=0; existing<s_fixed_label_count; ++existing) {
          const auto& other = s_fixed_labels[existing];
          const int other_w = plane_label_font::width(
              data::large_airports::kAirports[other.airport].ident,false);
          const AirportLabelRect old = {other.x,other.y,other.x+other_w,other.y+9};
          if (airportRectsOverlap(rect,old,3)) {overlaps=true; break;}
        }
        if (overlaps || airportLabelOverlapsRunway(rect)) continue;
        s_fixed_labels[s_fixed_label_count++] = {ap_idx,
            static_cast<int16_t>(rect.x0),static_cast<int16_t>(rect.y0)};
        placed = true;
        break;
      }
      if (placed) break;
    }
    // If no runway-free position is available, omit this airport name rather
    // than paint over a landing strip. Reconsider only when range changes.
  }
}

void drawFixedAirportLabels(lgfx::LGFXBase& gfx) {
  if (!s_fixed_label_count) return;
  // Our native 6x9 airport font is drawn directly; no VLW font loading.
  for (size_t i=0;i<s_fixed_label_count;++i) {
    const auto& label = s_fixed_labels[i];
    plane_label_font::draw(gfx,label.x,label.y,
        data::large_airports::kAirports[label.airport].ident,
        radar::kColorRunwayLabel,false);
  }
}

}  // namespace

void drawLargeAirportRunways(lgfx::LGFXBase& gfx) {
  if (!radar::showRunways()) return;
  // The cache key follows exactly the settings that alter runway projection.
  // Display colors/font choices do not affect geometry and remain live.
  const float range = radar::fetchRadiusKm();
  const double lat = services::location::lat();
  const double lon = services::location::lon();
  if (!s_cache_valid || s_cache_range != range ||
      s_cache_lat != lat || s_cache_lon != lon) {
    rebuildRunwayCache(lat, lon, range);
  }
  if (s_cache_overflow) {
    // Only on unusually dense maps: recompute the full correct set rather
    // than silently dropping runways past the fixed cache capacity.
    // Keep the overflow state cached; the fallback still draws every frame.
    const double center_cos = fabs(cos(lat * kDegToRad));
    const double safe_cos = center_cos > 0.0001 ? center_cos : 0.0001;
    const int64_t center_lat_e7 = static_cast<int64_t>(llround(lat * 1e7));
    const int64_t center_lon_e7 = static_cast<int64_t>(llround(lon * 1e7));
    const int64_t lat_limit = static_cast<int64_t>(ceil((range / kKmPerDeg + 0.001) * 1e7));
    const int64_t lon_limit = static_cast<int64_t>(ceil((range / (kKmPerDeg * safe_cos) + 0.001) * 1e7));
    memset(s_airport_state, 0, sizeof(s_airport_state));
    uint16_t label_airports[kMaxAirportLabels];
    size_t label_count = 0;
    for (size_t i = 0; i < data::large_airports::kRunwayCount; ++i) {
      const auto& rw = data::large_airports::kRunways[i];
      const uint16_t ap_idx = rw.airport_idx;
      uint8_t& state = s_airport_state[ap_idx];
      if (state == 0) {
        const auto& ap = data::large_airports::kAirports[ap_idx];
        const int64_t dlat = static_cast<int64_t>(ap.lat_e7) - center_lat_e7;
        int64_t dlon = static_cast<int64_t>(ap.lon_e7) - center_lon_e7;
        if (dlon > 1800000000LL) dlon -= 3600000000LL;
        if (dlon < -1800000000LL) dlon += 3600000000LL;
        if (llabs(dlat) > lat_limit || llabs(dlon) > lon_limit) state = 1;
        else {
          float dx = 0, dy = 0, d = 0;
          offsetKmFromCenter(e7ToDeg(ap.lat_e7), e7ToDeg(ap.lon_e7), &dx, &dy, &d);
          state = d <= range ? 2 : 1;
        }
      }
      if (state == 1) continue;
      CachedRunway line = {};
      if (!projectRunwayLine(rw, &line)) continue;
      drawProjectedRunway(gfx, line);
      if (state == 2 && label_count < kMaxAirportLabels) {
        state = 3;
        label_airports[label_count++] = ap_idx;
      }
    }
    // Airport-name positions are frozen for this range, even in overflow.
    drawFixedAirportLabels(gfx);
    return;
  }
  for (size_t i = 0; i < s_cached_runway_count; ++i) {
    drawProjectedRunway(gfx, s_cached_runways[i]);
  }
  drawFixedAirportLabels(gfx);
}

}  // namespace ui::runway
