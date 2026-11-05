#include "ui/radar_display.h"
#include "services/aircraft_style_settings.h"
#include "services/aircraft_palette.h"
#include "services/wifi_setup.h"
#include "ui/pixel_label_font.h"
#include "ui/aircraft_altitude_units.h"
#include "ui/radar_display_options.h"

#include <Arduino.h>
#include <esp32-hal-cpu.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstddef>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/aircraft_watchlist.h"
#include "ui/radar_range.h"
#include "ui/aircraft_trail_settings.h"
#include "ui/trail_fade.h"
#include "ui/trail_color_gradient.h"
#include "ui/trail_beam_profile.h"
#include "ui/trail_compositing.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"
#include "ui/turn_prediction.h"
#include "ui/aircraft_transition.h"

// Embedded read-only RGB332 grid bitmap (57,600 bytes in firmware flash).
// PlatformIO provides these symbols via board_build.embed_files.
extern "C" {
extern const uint8_t _binary_data_radar_grid_rgb332_start[] asm(
    "_binary_data_radar_grid_rgb332_start");
extern const uint8_t _binary_data_radar_grid_rgb332_end[] asm(
    "_binary_data_radar_grid_rgb332_end");
extern const uint8_t _binary_data_radar_classic_ppi_rgb332_start[] asm(
    "_binary_data_radar_classic_ppi_rgb332_start");
extern const uint8_t _binary_data_radar_classic_ppi_rgb332_end[] asm(
    "_binary_data_radar_classic_ppi_rgb332_end");
extern const uint8_t _binary_data_radar_minimal_rgb332_start[] asm(
    "_binary_data_radar_minimal_rgb332_start");
extern const uint8_t _binary_data_radar_minimal_rgb332_end[] asm(
    "_binary_data_radar_minimal_rgb332_end");
extern const uint8_t _binary_data_radar_polar_rgb332_start[] asm(
    "_binary_data_radar_polar_rgb332_start");
extern const uint8_t _binary_data_radar_polar_rgb332_end[] asm(
    "_binary_data_radar_polar_rgb332_end");
extern const uint8_t _binary_data_radar_dashed_rgb332_start[] asm(
    "_binary_data_radar_dashed_rgb332_start");
extern const uint8_t _binary_data_radar_dashed_rgb332_end[] asm(
    "_binary_data_radar_dashed_rgb332_end");
extern const uint8_t _binary_data_radar_super_minimal_rgb332_start[] asm(
    "_binary_data_radar_super_minimal_rgb332_start");
extern const uint8_t _binary_data_radar_super_minimal_rgb332_end[] asm(
    "_binary_data_radar_super_minimal_rgb332_end");
}

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorCenterDot = 0x03A0;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorMilitaryAircraft = 0x001F;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitude = 0xFFE0;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;

}  // namespace radar

namespace {

bool s_palette_ready = false;

struct FlashGridAsset {
  const uint8_t* start;
  const uint8_t* end;
};

FlashGridAsset currentFlashGridAsset() {
  switch (radar::backgroundPattern()) {
    case radar::BackgroundPattern::ClassicPpi:
      return {_binary_data_radar_classic_ppi_rgb332_start,
              _binary_data_radar_classic_ppi_rgb332_end};
    case radar::BackgroundPattern::Minimal:
      return {_binary_data_radar_minimal_rgb332_start,
              _binary_data_radar_minimal_rgb332_end};
    case radar::BackgroundPattern::PolarGrid:
      return {_binary_data_radar_polar_rgb332_start,
              _binary_data_radar_polar_rgb332_end};
    case radar::BackgroundPattern::DashedRings:
      return {_binary_data_radar_dashed_rgb332_start,
              _binary_data_radar_dashed_rgb332_end};
    case radar::BackgroundPattern::SuperMinimal:
      return {_binary_data_radar_super_minimal_rgb332_start,
              _binary_data_radar_super_minimal_rgb332_end};
    case radar::BackgroundPattern::Original:
    default:
      return {_binary_data_radar_grid_rgb332_start,
              _binary_data_radar_grid_rgb332_end};
  }
}

// 8-level brightness palette shared by every embedded pattern.
uint8_t s_bg_palette332[8] = {};

void backgroundThemeRgb(uint8_t& r, uint8_t& g, uint8_t& b) {
  switch (radar::backgroundTheme()) {
    case radar::BackgroundTheme::Red:    r = 255; g =   0; b =   0; break;
    case radar::BackgroundTheme::Amber:  r = 255; g = 176; b =   0; break;
    case radar::BackgroundTheme::Cyan:   r =   0; g = 240; b = 255; break;
    case radar::BackgroundTheme::Violet: r = 178; g = 105; b = 255; break;
    case radar::BackgroundTheme::White:  r = 240; g = 240; b = 240; break;
    case radar::BackgroundTheme::Green:
    default:                            r =   0; g = 255; b =   0; break;
  }
}

uint16_t themePrimaryColor565(uint8_t brightness) {
  uint8_t r, g, b;
  backgroundThemeRgb(r, g, b);
  r = static_cast<uint8_t>((static_cast<unsigned>(r) * brightness + 127) / 255);
  g = static_cast<uint8_t>((static_cast<unsigned>(g) * brightness + 127) / 255);
  b = static_cast<uint8_t>((static_cast<unsigned>(b) * brightness + 127) / 255);
  return config::kDisplayRgbOrder ? tft.color565(b, g, r)
                                  : tft.color565(r, g, b);
}

void updateBackgroundPalette() {
  uint8_t r, g, b;
  backgroundThemeRgb(r, g, b);
  for (unsigned level = 0; level < 8; ++level) {
    // Round once on palette initialization, not per pixel/frame.
    const uint8_t r3 = static_cast<uint8_t>((r * level + 127) / 255);
    const uint8_t g3 = static_cast<uint8_t>((g * level + 127) / 255);
    const uint8_t b2 = static_cast<uint8_t>((b * level * 3 + 892) / 1785);
    s_bg_palette332[level] = static_cast<uint8_t>((r3 << 5) | (g3 << 2) | b2);
  }
}

uint16_t s_ring_colors[4] = {};
uint16_t s_cross_color = 0;
uint16_t s_scale_label_color = 0;
bool s_label_metrics_ready = false;
bool s_cardinal_use_vlw = false;
bool s_scale_use_vlw = false;
float s_cardinal_vlw_size = 0.56f;
float s_scale_vlw_size = 0.50f;
float s_tag_vlw_size = 0.56f;
const lgfx::GFXfont* s_cardinal_gfx = &fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_tag_gfx = &fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;
bool s_tag_use_vlw = false;

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

lgfx::LovyanGFX* s_draw = &tft;
LGFX_Sprite s_frame(&tft);  // The ONE full-size 240x240 RGB332 framebuffer.
bool s_frame_ready = false;
bool s_frame_attempted = false;
// Complete redraw into one RGB332 framebuffer.
// No separate fast/slow layers, icon erasing or secondary graphic buffers.
bool s_scene_dirty = true;
bool s_last_scene_dynamic = false;
bool s_last_countdown_active = false;
uint32_t s_last_frame_us = 0;  // Full draw + SPI transfer.
uint32_t s_last_scene_us = 0;  // Drawing only, excluding SPI transfer.
RadarFrameTimings s_stage_timing;
RadarSpiSlowDiag s_spi_slow_diag;
// -1 = unchecked, 0 = valid flash asset, 1 = fallback to direct grid.
int8_t s_flash_grid_fallback = -1;
constexpr size_t kFlashGridBytes = radar::kSize * radar::kSize;



class DrawScope {
 public:
  explicit DrawScope(lgfx::LovyanGFX& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  lgfx::LovyanGFX* prev_;
};

int absDiff(int a, int b) { return std::abs(a - b); }

int measureGfxHeight(const lgfx::GFXfont& font) {
  tft.setFont(&font);
  tft.setTextSize(1);
  return tft.fontHeight();
}

int measureVlwHeight(float size) {
  tft.setTextSize(size);
  return tft.fontHeight();
}

float findVlwSizeForHeight(int target_px) {
  float lo = 0.25f;
  float hi = 1.2f;
  for (int i = 0; i < 16; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void applyScaleStyle();

const lgfx::GFXfont* pickGfxFontClosest(
    int target_px, const lgfx::GFXfont* const* candidates, size_t count) {
  const lgfx::GFXfont* best = candidates[0];
  int best_diff = absDiff(measureGfxHeight(*best), target_px);

  for (size_t i = 1; i < count; ++i) {
    const int diff = absDiff(measureGfxHeight(*candidates[i]), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = candidates[i];
    }
  }
  return best;
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  const int cardinal_target = radar::kCardinalLabelHeightPx;

  if (displayFontIsSmooth()) {
    s_cardinal_use_vlw = true;
    s_cardinal_vlw_size = findVlwSizeForHeight(cardinal_target);
    const int cardinal_h = measureVlwHeight(s_cardinal_vlw_size);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    s_scale_use_vlw = true;
    s_scale_vlw_size = findVlwSizeForHeight(scale_target);
  } else {
    const lgfx::GFXfont* cardinal_candidates[] = {&fonts::FreeSansBold12pt7b,
                                                  &fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(cardinal_target, cardinal_candidates, 2);
    s_cardinal_use_vlw = false;

    const int cardinal_h = measureGfxHeight(*s_cardinal_gfx);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    const lgfx::GFXfont* scale_candidates[] = {&fonts::FreeSansBold9pt7b,
                                               &fonts::FreeSansBold12pt7b};
    s_scale_gfx = pickGfxFontClosest(scale_target, scale_candidates, 2);
    s_scale_use_vlw = false;
  }

  applyScaleStyle();
  s_scale_label_h = tft.fontHeight();
  s_scale_label_max_w = 0;
  char label[12];
  for (size_t i = 0; i < radar::kRangePresetCount; ++i) {
    for (bool miles : {false, true}) {
      radar::formatRing3Label(label, sizeof(label), radar::kRangePresets[i].ring3_km,
                              miles);
      const int w = tft.textWidth(label);
      if (w > s_scale_label_max_w) {
        s_scale_label_max_w = w;
      }
    }
  }

  s_label_metrics_ready = true;
}

void initTagLabelMetrics() {
  if (s_tag_label_metrics_ready) {
    return;
  }

  const int target = radar::kAircraftTagLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_tag_use_vlw = true;
    s_tag_vlw_size = findVlwSizeForHeight(target);
  } else {
    const lgfx::GFXfont* tag_candidates[] = {&fonts::FreeSansBold12pt7b,
                                               &fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(target, tag_candidates, 2);
    s_tag_use_vlw = false;
  }

  s_tag_label_metrics_ready = true;
}

void initPalette() {
  // Palette only changes when the General page changes theme/pattern.
  if (s_palette_ready) return;
  updateBackgroundPalette();
  radar::kColorBackground = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  radar::kColorGrid = tft.color565(radar::kGridR, radar::kGridG, radar::kGridB);
  radar::kColorLabel = tft.color565(255, 255, 255);
  radar::kColorCenter = themePrimaryColor565(144);
  radar::kColorCenterDot = themePrimaryColor565(146);
  // GC9A01 BGR panel: swap R/B in color565 so logical red renders red on screen.
  if (config::kDisplayRgbOrder) {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftB, radar::kAircraftG, radar::kAircraftR);
    radar::kColorMilitaryAircraft =
        tft.color565(radar::kMilitaryAircraftB, radar::kMilitaryAircraftG, radar::kMilitaryAircraftR);
  } else {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
    radar::kColorMilitaryAircraft =
        tft.color565(radar::kMilitaryAircraftR, radar::kMilitaryAircraftG, radar::kMilitaryAircraftB);
  }
  // Medium neutral RGB332 gray for secondary type and altitude information.
  radar::kColorTagType = tft.color565(146, 146, 170);
  radar::kColorTagAltitude = radar::kColorTagType;
  radar::kColorRunway =
      tft.color565(radar::kRunwayR, radar::kRunwayG, radar::kRunwayB);
  radar::kColorRunwayLabel = tft.color565(radar::kRunwayLabelR, radar::kRunwayLabelG,
                                          radar::kRunwayLabelB);
  const uint8_t primary[4] = {112, 112, 80, 45};
  for (int j = 0; j < 4; ++j) {
    s_ring_colors[j] = themePrimaryColor565(primary[j]);
  }
  s_cross_color = themePrimaryColor565(45);
  s_scale_label_color = themePrimaryColor565(
      radar::backgroundPattern() == radar::BackgroundPattern::SuperMinimal ? 48 : 64);
  s_palette_ready = true;
}

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;

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


float innerRingMaxKm() {
  // Preserve scale: the outer radar ring is the selected range. Aircraft
  // continue beyond that ring until close to the physical display edge.
  return radar::rangeCurrent().outer_km *
         static_cast<float>(radar::kAircraftDisplayRadiusPx) /
         static_cast<float>(radar::kGridOuterRadius);
}

/** Aircraft visibility limit, in kilometres, for the selected range. */
bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

// Explicit watchlist color overrides civilian and military defaults.
// Otherwise use the configured military/helicopter/normal defaults.
services::aircraft_palette::Selection aircraftColorSelection(
    const services::adsb::Aircraft& ac) {
  uint32_t rgb = 0;
  uint8_t hue = 0, shade = 102;
  if (services::watchlist::find(ac, &rgb, nullptr, 0, nullptr, &hue, &shade)) {
    services::aircraft_palette::Selection selected;
    selected.rgb = rgb;
    selected.hue = hue;
    selected.shade = shade;
    return selected;
  }
  return services::aircraft_styles::defaultSelection(ac);
}

uint16_t color565ForRgb(uint32_t rgb) {
  const uint8_t r = static_cast<uint8_t>(rgb >> 16);
  const uint8_t g = static_cast<uint8_t>(rgb >> 8);
  const uint8_t b = static_cast<uint8_t>(rgb);
  return config::kDisplayRgbOrder ? tft.color565(b, g, r) : tft.color565(r, g, b);
}
uint16_t watchedAircraftColor(const services::adsb::Aircraft& ac, uint16_t fallback) {
  (void)fallback;
  return color565ForRgb(aircraftColorSelection(ac).rgb);
}

// The aircraft icon always retains its original brightest selected color.
// Keep Q8 (8 fractional bits) palette positions for every stored fix.
// The rasterizer interpolates PALETTE POSITIONS across each segment and samples
// the palette at each covered pixel. This avoids RGB565 endpoint quantization
// causing a whole short segment to jump between colors at once.
// Same 32 * 2 bytes as the previous endpoint RGB565 table, no extra buffer.
struct TrailShadeLut {
  uint16_t shades_q8[32] = {};
  uint16_t at(size_t age) const { return shades_q8[age < 32 ? age : 31]; }
};
// Edge dots shrink smoothly toward the 2x-ring3
// ADS-B search limit. Farther dots are drawn first, larger/nearer on top.
float beyondDotRadiusKm(float distance_km) {
  const float near_km = radar::rangeCurrent().outer_km *
      (static_cast<float>(radar::kAircraftDisplayRadiusPx) /
       static_cast<float>(radar::kGridOuterRadius));
  const float far_km = radar::fetchRadiusKm();
  const float denominator = std::max(0.1f, far_km - near_km);
  float fraction = (distance_km - near_km) / denominator;
  fraction = std::max(0.0f, std::min(1.0f, fraction));
  // Radii 4.0 -> 1.4 pixels; ease-out highlights nearby out-of-range targets.
  const float eased = fraction * fraction * (3.0f - 2.0f * fraction);
  return 4.0f - 2.6f * eased;
}

// Q4 pixel coordinates. One pixel = 16 units, giving exact
// 4x4 antialias sample positions 2,6,10,14 without any per-sample floats.
// Only convert aircraft geometry ONCE per primitive; the geographic maths
// and color blending format remain unchanged.
constexpr int kRasterShift = 4;
constexpr int kRasterOne = 1 << kRasterShift;
constexpr int kRasterSample[4] = {2, 6, 10, 14};
constexpr int kBlendShift = 12;
constexpr int kBlendOne = 1 << kBlendShift;

inline int32_t rasterQ4(float value) {
  return static_cast<int32_t>(lroundf(value * kRasterOne));
}

// Saturating additive RGB565. Alpha is geometric 4x4 coverage only;
// there is no age-opacity blending. Clamp channels to avoid wraparound.
inline uint16_t addRgb565CoverageQ12(uint16_t bg, uint16_t color,
                                     int32_t coverage) {
  coverage = std::max<int32_t>(0, std::min<int32_t>(4096, coverage));
  return ui::trail_compositing::addCoverage565(
      bg, color, static_cast<uint16_t>(coverage));
}
// Fractional palette position interpolation, fixed-point only.
// The caller supplies an 0..4096 segment fraction. The result remains Q8
// until palette sampling; no RGB565 endpoint blend is performed.
inline uint16_t trailColorAtFractionQ12(uint8_t hue, uint16_t start_q8,
                                       uint16_t end_q8, uint16_t fraction_q12) {
  const uint16_t shade_q8 = ui::trail_color_gradient::interpolateShadeQ8(
      start_q8, end_q8, fraction_q12);
  return color565ForRgb(services::aircraft_palette::rgb888SmoothAtQ8(
      hue, shade_q8));
}

// Add symbol light to the existing RGB332 scene. Brightness is applied
// before compositing, so dim aircraft cannot overwrite brighter trails.
// Only the existing framebuffer is touched; no per-icon scratch buffer.
void drawIconPixelAdditive(int x, int y, uint16_t color, unsigned coverage,
                           uint16_t brightness_q12) {
  if (!coverage || !brightness_q12 || x < 0 || y < 0 ||
      x >= radar::kSize || y >= radar::kSize) return;
  const uint16_t contribution_q12 = static_cast<uint16_t>(
      (static_cast<uint32_t>(std::min(coverage, 16u)) * brightness_q12 + 8u) >> 4);
  if (!contribution_q12) return;
  const uint16_t bg = s_draw->readPixel(x, y);
  s_draw->drawPixel(x, y,
      ui::trail_compositing::addCoverage565(bg, color, contribution_q12));
}

// Four-by-four coverage avoids size and position snapping when the rim
// bearing or the chosen dot radius changes fractionally between frames.
// Identical 4x4 sample layout, but the inner coverage loop is Q4 integer.
void drawBeyondRingDot(float cx, float cy, float radius, uint16_t color,
                       uint16_t brightness_q12) {
  const int x0 = std::max(0, static_cast<int>(floorf(cx - radius - 1.0f)));
  const int x1 = std::min(radar::kSize - 1,
                          static_cast<int>(ceilf(cx + radius + 1.0f)));
  const int y0 = std::max(0, static_cast<int>(floorf(cy - radius - 1.0f)));
  const int y1 = std::min(radar::kSize - 1,
                          static_cast<int>(ceilf(cy + radius + 1.0f)));
  const int32_t center_x = rasterQ4(cx);
  const int32_t center_y = rasterQ4(cy);
  // Quantize the radius SQUARED, not the radius, for smoother size changes.
  const int32_t radius_sq = static_cast<int32_t>(
      lroundf(radius * radius * (kRasterOne * kRasterOne)));
  for (int y = y0; y <= y1; ++y) {
    const int32_t py = y << kRasterShift;
    for (int x = x0; x <= x1; ++x) {
      const int32_t px = x << kRasterShift;
      unsigned coverage = 0;
      for (int sy = 0; sy < 4; ++sy) {
        const int32_t dy = py + kRasterSample[sy] - center_y;
        const int32_t dy_sq = dy * dy;
        for (int sx = 0; sx < 4; ++sx) {
          const int32_t dx = px + kRasterSample[sx] - center_x;
          if (dx * dx + dy_sq <= radius_sq) ++coverage;
        }
      }
      if (coverage) drawIconPixelAdditive(x, y, color, coverage, brightness_q12);
    }
  }
}

// Rotorcraft: anti-aliased triangle plus a snug circular ring.
// Radius equals nose length: the circle and triangle tip meet exactly.
void drawHelicopterRing(float cx, float cy, uint16_t color,
                        uint16_t brightness_q12) {
  const float scale = ui::display_options::iconScale();
  const float radius = radar::kAircraftNoseLenPx * scale * 0.85f - 0.65f;
  constexpr float halfWidth = 0.48f;
  const float outer = radius + halfWidth;
  const float inner = std::max(0.0f, radius - halfWidth);
  const int32_t outer_sq = static_cast<int32_t>(lroundf(
      outer * outer * (kRasterOne * kRasterOne)));
  const int32_t inner_sq = static_cast<int32_t>(lroundf(
      inner * inner * (kRasterOne * kRasterOne)));
  const int32_t center_x = rasterQ4(cx);
  const int32_t center_y = rasterQ4(cy);
  const int x0 = std::max(0, static_cast<int>(floorf(cx - radius - 1.0f)));
  const int x1 = std::min(radar::kSize-1,
                          static_cast<int>(ceilf(cx + radius + 1.0f)));
  const int y0 = std::max(0, static_cast<int>(floorf(cy - radius - 1.0f)));
  const int y1 = std::min(radar::kSize-1,
                          static_cast<int>(ceilf(cy + radius + 1.0f)));
  for (int y = y0; y <= y1; ++y) {
    const int32_t py = y << kRasterShift;
    for (int x = x0; x <= x1; ++x) {
      const int32_t px = x << kRasterShift;
      unsigned cov = 0;
      for (int sy = 0; sy < 4; ++sy) {
        const int32_t dy = py + kRasterSample[sy] - center_y;
        const int32_t dy_sq = dy * dy;
        for (int sx = 0; sx < 4; ++sx) {
          const int32_t dx = px + kRasterSample[sx] - center_x;
          const int32_t d_sq = dx * dx + dy_sq;
          if (d_sq >= inner_sq && d_sq <= outer_sq) ++cov;
        }
      }
      if (!cov) continue;
      drawIconPixelAdditive(x, y, color, cov, brightness_q12);
    }
  }
}

// Q4 fixed-point triangle edges. Retains the original 16-sample AA
// coverage, and only the three vertex coordinates are rounded to 1/16 px.
inline int32_t triEdgeQ4(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                         int32_t px, int32_t py) {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void drawHeadingTriangle(float cx, float cy, float heading_deg,
                         uint16_t color, uint16_t brightness_q12) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  const float sh = sinf(rad);
  const float ch = cosf(rad);
  const float scale = ui::display_options::iconScale();
  const float tx = cx + sh * radar::kAircraftNoseLenPx * scale;
  const float ty = cy - ch * radar::kAircraftNoseLenPx * scale;
  const float bx = cx - sh * radar::kAircraftTailLenPx * scale;
  const float by = cy + ch * radar::kAircraftTailLenPx * scale;
  const float wx = ch * radar::kAircraftTailHalfPx * scale;
  const float wy = sh * radar::kAircraftTailHalfPx * scale;
  const float ax = bx + wx, ay = by + wy;
  const float bx2 = bx - wx, by2 = by - wy;
  const int32_t txq = rasterQ4(tx), tyq = rasterQ4(ty);
  const int32_t axq = rasterQ4(ax), ayq = rasterQ4(ay);
  const int32_t bxq = rasterQ4(bx2), byq = rasterQ4(by2);
  const int32_t signed_area = triEdgeQ4(txq, tyq, axq, ayq, bxq, byq);
  if (signed_area == 0) return;
  const bool positive = signed_area > 0;
  const int min_x = std::max(0, static_cast<int>(floorf(std::min(tx, std::min(ax, bx2)))));
  const int min_y = std::max(0, static_cast<int>(floorf(std::min(ty, std::min(ay, by2)))));
  const int max_x = std::min(radar::kSize - 1,
                              static_cast<int>(ceilf(std::max(tx, std::max(ax, bx2)))));
  const int max_y = std::min(radar::kSize - 1,
                              static_cast<int>(ceilf(std::max(ty, std::max(ay, by2)))));
  bool drew_pixel = false;
  for (int py = min_y; py <= max_y; ++py) {
    const int32_t row_y = py << kRasterShift;
    for (int px = min_x; px <= max_x; ++px) {
      const int32_t row_x = px << kRasterShift;
      unsigned coverage = 0;
      for (int sy = 0; sy < 4; ++sy) {
        const int32_t yy = row_y + kRasterSample[sy];
        for (int sx = 0; sx < 4; ++sx) {
          const int32_t xx = row_x + kRasterSample[sx];
          const int32_t e0 = triEdgeQ4(txq, tyq, axq, ayq, xx, yy);
          const int32_t e1 = triEdgeQ4(axq, ayq, bxq, byq, xx, yy);
          const int32_t e2 = triEdgeQ4(bxq, byq, txq, tyq, xx, yy);
          if (positive ? (e0 >= 0 && e1 >= 0 && e2 >= 0)
                       : (e0 <= 0 && e1 <= 0 && e2 <= 0)) ++coverage;
        }
      }
      if (!coverage) continue;
      drew_pixel = true;
      drawIconPixelAdditive(px, py, color, coverage, brightness_q12);
    }
  }
  if (!drew_pixel && scale <= 0.2f) {
    drawIconPixelAdditive(static_cast<int>(lroundf(cx)),
                          static_cast<int>(lroundf(cy)), color, 16u,
                          brightness_q12);
  }
}

// Native 6x8 fixed-width bitmap font; glyphs remain pixel-aligned.
void applyTagStyle() {
  s_draw->setFont(&fonts::Font0);
  s_draw->setTextSize(1);
}

// Each enabled aircraft field gets its own line in a single text column.
// In particular, type no longer shares a row with altitude. This keeps
// registration/callsign/type/altitude aligned for both left- and right-side tags.
struct AircraftTagText {
  char custom[17] = {};
  char callsign[24] = {};
  char registration[24] = {};
  char type[8] = {};
  char altitude[20] = {};
};

AircraftTagText aircraftTagText(const services::adsb::Aircraft& plane) {
  AircraftTagText tag;
  // Watchlist title is an ADDITIONAL line, never a replacement callsign.
  services::watchlist::find(plane, nullptr, tag.custom, sizeof(tag.custom));
  if (ui::display_options::show(ui::display_options::kCallsign)) {
    snprintf(tag.callsign, sizeof(tag.callsign), "%s", plane.callsign);
  }
  if (ui::display_options::show(ui::display_options::kRegistration)) {
    snprintf(tag.registration, sizeof(tag.registration), "%s", plane.registration);
  }
  if (tag.callsign[0] && strcmp(tag.callsign, tag.registration) == 0) {
    tag.registration[0] = '\0';
  }
  if (ui::display_options::show(ui::display_options::kType)) {
    snprintf(tag.type, sizeof(tag.type), "%s", plane.type);
  }
  if (ui::display_options::show(ui::display_options::kAltitude) && plane.alt[0]) {
    if (ui::altitude_units::meters() && strstr(plane.alt, "ft")) {
      const long feet = atol(plane.alt);
      const long metres = lroundf(static_cast<float>(feet) * 0.3048f);
      snprintf(tag.altitude, sizeof(tag.altitude), "%ldm", metres);
    } else {
      snprintf(tag.altitude, sizeof(tag.altitude), "%s", plane.alt);
    }
  }
  return tag;
}

// Adaptive detail is ALWAYS enabled. The web checkboxes control which lines
// may appear; this layout only removes already-enabled lines when necessary.
// Levels: 0=all, 1=no type, 2=no altitude, 3=no registration,
// 4=callsign/name using native detail (smaller) pixel font.
struct TagLayout {
  AircraftTagText text = {};
  uint8_t level = 0;
  bool primary = false;
  int width = 0;
  int height = 0;
};

TagLayout tagLayoutFor(const AircraftTagText& original, uint8_t level) {
  TagLayout out = {};
  out.text = original;
  out.level = level;
  out.primary = radar::rangeCurrent().ring3_km < 50.0f && level != 4;
  if (level >= 1) out.text.type[0] = '\0';
  if (level >= 2) out.text.altitude[0] = '\0';
  if (level >= 3) out.text.registration[0] = '\0';
  if (level == 4 && !original.custom[0] && !original.callsign[0]) {
    // Respect a disabled callsign checkbox: the final compact label can
    // instead retain one of the other *enabled* fields in its original color.
    if (original.registration[0]) {
      memcpy(out.text.registration, original.registration,
             sizeof(out.text.registration));
    } else if (original.altitude[0]) {
      memcpy(out.text.altitude, original.altitude,
             sizeof(out.text.altitude));
    } else if (original.type[0]) {
      memcpy(out.text.type, original.type, sizeof(out.text.type));
    }
  }
  const int line_h = out.primary ? 11 : 9;
  auto measure = [&](const char* str, bool big, int height) {
    if (!str[0]) return;
    out.width = std::max(out.width, plane_label_font::width(str, big));
    out.height += height;
  };
  measure(out.text.custom, out.primary, line_h);
  measure(out.text.callsign, out.primary, line_h);
  measure(out.text.registration, out.primary, line_h);
  measure(out.text.type, false, 9);
  measure(out.text.altitude, false, 9);
  return out;
}

uint16_t dim565(uint16_t color, uint16_t brightness_q12);

void drawAircraftTag(int anchor_x, int ly, bool tag_on_right,
                     const TagLayout& tag, uint16_t custom_color,
                     uint16_t effective_brightness_q12) {
  int y = ly;
  auto drawLine = [&](const char* txt, uint16_t color, bool large, int height) {
    if (!txt[0]) return;
    const int w = plane_label_font::width(txt, large);
    const uint16_t dimmed = dim565(color, effective_brightness_q12);
    if (dimmed) plane_label_font::draw(*s_draw,
                    tag_on_right ? anchor_x : anchor_x - w,
                    y, txt, dimmed, large);
    y += height;
  };
  const int line_h = tag.primary ? 11 : 9;
  drawLine(tag.text.custom, custom_color, tag.primary, line_h);
  drawLine(tag.text.callsign, radar::kColorLabel, tag.primary, line_h);
  drawLine(tag.text.registration, radar::kColorLabel, tag.primary, line_h);
  drawLine(tag.text.type, radar::kColorTagType, false, 9);
  drawLine(tag.text.altitude, radar::kColorTagAltitude, false, 9);
}

// A small number of aircraft may keep fading after a successful fetch removes
// them. Statically allocated: no per-frame heap allocation and no new sprite.
constexpr size_t kFadingGhostCapacity = 16;
constexpr size_t kVisualCapacity = services::adsb::kMaxAircraft + kFadingGhostCapacity;
void formatTrailIdentity(const services::adsb::Aircraft& plane, char* dst,
                         size_t dst_len);
struct VisualEntry {
  char identity[16] = {};
  services::adsb::Aircraft last_aircraft = {};
  ui::turn_prediction::Pose last_pose = {};
  uint32_t transition_ms = 0;
  uint16_t transition_from = 0;
  bool present = false;
};
static VisualEntry s_visuals[services::adsb::kMaxAircraft] = {};
static uint32_t s_visual_last_fetch = 0;

uint16_t visualIntensity(const VisualEntry& v, uint32_t now) {
  return ui::aircraft_transition::intensity(v.present,
       static_cast<uint32_t>(now - v.transition_ms), v.transition_from,
       ui::trail_fade::activeCurveQ12());
}

VisualEntry* visualFor(const char* id) {
  if (!id[0]) return nullptr;
  for (VisualEntry& v : s_visuals)
    if (strcmp(v.identity, id) == 0) return &v;
  return nullptr;
}

// Only called on a NEW successful ADS-B snapshot, never per-frame and never
// after a failed HTTP request. A disappearing icon fades immediately from the
// successful fetch that first omits it.
void updateVisualCache(const services::adsb::Aircraft* feed, size_t count,
                       uint32_t fetch_ms) {
  if (!fetch_ms || fetch_ms == s_visual_last_fetch) return;
  s_visual_last_fetch = fetch_ms;
  // Keep the 1 KB identity scratch out of the ESP32-C3 loop-task stack.
  static char ids[services::adsb::kMaxAircraft][16];
  memset(ids, 0, sizeof(ids));
  for (size_t i = 0; i < count; ++i)
    formatTrailIdentity(feed[i], ids[i], sizeof(ids[i]));

  for (VisualEntry& v : s_visuals) {
    if (!v.identity[0] || !v.present) continue;
    bool found = false;
    for (size_t i = 0; i < count; ++i)
      if (ids[i][0] && strcmp(v.identity, ids[i]) == 0) { found = true; break; }
    if (!found) {
      v.transition_from = visualIntensity(v, fetch_ms);
      v.transition_ms = fetch_ms;
      v.present = false;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (!ids[i][0]) continue;
    VisualEntry* v = visualFor(ids[i]);
    if (!v) {
      // Prefer genuinely free or fully faded slots. A full feed must not
      // evict a still-visible aircraft just to preserve a ghost.
      for (VisualEntry& slot : s_visuals) {
        if (!slot.identity[0] ||
            (!slot.present && visualIntensity(slot, fetch_ms) == 0)) {
          v = &slot; break;
        }
      }
      if (!v) {
        for (VisualEntry& slot : s_visuals) {
          if (!slot.present) { v = &slot; break; }
        }
      }
      if (!v) continue;
      *v = VisualEntry{};
      strncpy(v->identity, ids[i], sizeof(v->identity) - 1);
      v->transition_from = 0;
      v->transition_ms = fetch_ms;
    } else if (!v->present) {
      // Returning during fade-out: brighten from CURRENT visual level.
      v->transition_from = visualIntensity(*v, fetch_ms);
      v->transition_ms = fetch_ms;
    }
    v->present = true;
    v->last_aircraft = feed[i];
  }
}

bool hasFadingGhost(uint32_t now) {
  for (const VisualEntry& v : s_visuals)
    if (v.identity[0] && !v.present && visualIntensity(v, now) > 0)
      return true;
  return false;
}

uint16_t dim565(uint16_t color, uint16_t brightness_q12) {
  if (brightness_q12 >= 4096u) return color;
  if (brightness_q12 == 0) return 0;
  const uint32_t r = ((color >> 11) & 31u) * brightness_q12;
  const uint32_t g = ((color >> 5) & 63u) * brightness_q12;
  const uint32_t b = (color & 31u) * brightness_q12;
  return static_cast<uint16_t>((((r + 2048u) >> 12) << 11) |
      (((g + 2048u) >> 12) << 5) | ((b + 2048u) >> 12));
}

struct AircraftDrawItem {
  size_t index = 0;
  float x = 0.0f;
  float y = 0.0f;
  int dist_sq = 0;
};

struct BeyondDotDrawItem {
  size_t index = 0;
  float x = 0.0f;
  float y = 0.0f;
  float radius = 4.0f;
  int dist_sq = 0;  // physical distance, not projected rim distance
};

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

// Retain per-aircraft label placements across frames.
// The text itself remains glyph-antialiased and pixel-aligned for legibility,
// while positions and layout are computed from fractional aircraft coordinates.
struct LabelRect { int x0, y0, x1, y1; };
struct StableLabel {
  uint32_t key = 0;
  float x = 0.0f;
  float y = 0.0f;
  uint32_t seen_ms = 0;
  uint32_t switched_ms = 0;
  uint32_t detail_changed_ms = 0;
  uint8_t slot = 255;
  uint8_t detail = 0;
  bool used = false;
  bool visible = false;
};
static StableLabel s_labelMemory[kVisualCapacity];

uint32_t labelIdentity(const services::adsb::Aircraft& p) {
  uint32_t h = 2166136261u;
  const char* text = p.callsign[0] ? p.callsign : p.type;
  while (*text) {
    h ^= static_cast<uint8_t>(*text++);
    h *= 16777619u;
  }
  return h;
}

int labelMemoryFor(const services::adsb::Aircraft& p, float x, float y,
                   uint32_t now, bool* occupied) {
  const uint32_t key = labelIdentity(p);
  const bool unique_callsign = p.callsign[0] != '\0';
  int candidate = -1;
  float closest = 999999.0f;
  int reuse = -1;
  uint32_t longest_idle = 0;
  for (size_t i = 0; i < kVisualCapacity; ++i) {
    StableLabel& m = s_labelMemory[i];
    if (!m.used || (now - m.seen_ms) > 20000UL) {
      if (reuse < 0) reuse = static_cast<int>(i);
      continue;
    }
    if (!occupied[i] && m.key == key) {
      const float dx = m.x - x;
      const float dy = m.y - y;
      const float distance = dx * dx + dy * dy;
      if ((unique_callsign || distance < 24.0f * 24.0f) && distance < closest) {
        candidate = static_cast<int>(i);
        closest = distance;
      }
    }
    if (!occupied[i] && (now - m.seen_ms) > longest_idle) {
      longest_idle = now - m.seen_ms;
      if (reuse < 0) reuse = static_cast<int>(i);
    }
  }
  const int index = candidate >= 0 ? candidate : reuse;
  if (index < 0) return -1;
  StableLabel& m = s_labelMemory[index];
  if (candidate < 0) {
    m = StableLabel{};
    m.key = key;
    m.used = true;
  }
  m.seen_ms = now;
  m.x = x;
  m.y = y;
  occupied[index] = true;
  return index;
}

bool rectIntersectsGap(const LabelRect& a, const LabelRect& b, int gap) {
  return a.x0 < b.x1 + gap && b.x0 < a.x1 + gap &&
         a.y0 < b.y1 + gap && b.y0 < a.y1 + gap;
}

bool rectFitsRoundScreen(const LabelRect& r) {
  if (r.x0 < 1 || r.y0 < 1 || r.x1 > radar::kSize - 1 ||
      r.y1 > radar::kSize - 1) return false;
  constexpr int radius = 118;
  constexpr int radius_sq = radius * radius;
  const int xs[2] = {r.x0, r.x1};
  const int ys[2] = {r.y0, r.y1};
  for (int x : xs) for (int y : ys) {
    const int dx = x - radar::kCenterX;
    const int dy = y - radar::kCenterY;
    if (dx * dx + dy * dy > radius_sq) return false;
  }
  return true;
}

// Six positions, right/left of symbol and centered/above/below. The gap is
// measured from the actual icon extent, not an overly large safety rectangle.
LabelRect tagSlotRect(float px, float py, int w, int h, int slot) {
  const int symbol_r = static_cast<int>(ceilf(
      radar::kAircraftNoseLenPx * ui::display_options::iconScale()));
  const bool on_right = slot < 3;
  const int horizontal = symbol_r + radar::kAircraftLabelGapPx + 1;
  const int anchor = static_cast<int>(lroundf(px + (on_right ? horizontal : -horizontal)));
  const int top = slot % 3 == 0 ? static_cast<int>(lroundf(py - 0.5f * h))
                : slot % 3 == 1 ? static_cast<int>(lroundf(py - symbol_r - h - 1))
                                : static_cast<int>(lroundf(py + symbol_r + 1));
  return {on_right ? anchor : anchor - w, top,
          on_right ? anchor + w : anchor, top + h};
}

void placeAircraftTags(const AircraftDrawItem* items, size_t count,
                       const services::adsb::Aircraft* planes,
                       size_t active_count) {
  initTagLabelMetrics();
  applyTagStyle();
  const int icon_r = static_cast<int>(ceilf(
      radar::kAircraftNoseLenPx * ui::display_options::iconScale()));
  static LabelRect reserved[kVisualCapacity];
  size_t reserved_count = 0;
  bool occupied[kVisualCapacity] = {};
  bool was_visible[kVisualCapacity] = {};
  int memory_index[kVisualCapacity];
  const uint32_t now = millis();
  for (size_t d = 0; d < count; ++d) {
    memory_index[d] = -1;
    // Disappearing aircraft retain only their icon, never their label.
    if (items[d].index >= active_count) continue;
    const int idx = labelMemoryFor(planes[items[d].index], items[d].x,
                                   items[d].y, now, occupied);
    memory_index[d] = idx;
    if (idx >= 0) was_visible[d] = s_labelMemory[idx].visible;
  }
  for (size_t d = 0; d < count; ++d) {
    if (memory_index[d] >= 0) s_labelMemory[memory_index[d]].visible = false;
  }
  // The number and POSITION of neighboring labels determine fitting.
  // Protected watched labels are fitted first; category priority follows.
  for (int priority = 0; priority < 6; ++priority) {
    const int class_pass = priority / 2;
    const bool old_pass = (priority % 2) == 0;
    for (size_t d = count; d-- > 0;) {
      if (items[d].index >= active_count) continue;
      const auto& p = planes[items[d].index];
      const bool watched_protected = services::watchlist::priority(p);
      const bool category_protected =
          (p.isMilitary && services::aircraft_styles::militaryPriority()) ||
          (p.isHelicopter && services::aircraft_styles::helicopterPriority());
      const int group = watched_protected ? 0 : (category_protected ? 1 : 2);
      if (group != class_pass || was_visible[d] != old_pass) continue;
      const bool protected_label = group < 2;
      const int idx = memory_index[d];
      if (idx < 0) continue;
      StableLabel& memory = s_labelMemory[idx];
      const AircraftTagText original = aircraftTagText(p);
      // Only watched targets receive a colored title. The color is resolved
      // through the same palette path used to draw the aircraft symbol.
      const uint16_t custom_color = watchedAircraftColor(p, radar::kColorLabel);
      const uint16_t effective_brightness_q12 = static_cast<uint16_t>(
          static_cast<unsigned>(ui::display_options::labelBrightnessPct()) *
          4096u / 100u);
      if (!effective_brightness_q12) continue;
      const bool preferred_right = items[d].x < radar::kCenterX;
      const int preferred = preferred_right ? 0 : 3;
      const int other = preferred_right ? 3 : 0;
      const int order[6] = {preferred, preferred + 1, preferred + 2,
                            other, other + 1, other + 2};
      // Retain a reduced layout for 2 seconds before attempting to expand it,
      // preventing flicker when one extra line barely fits between frames.
      const int start_level = (memory.detail > 0 &&
          static_cast<uint32_t>(now - memory.detail_changed_ms) < 2000u)
          ? memory.detail : 0;
      bool placed = false;
      for (int level = start_level; level <= 4 && !placed; ++level) {
        const TagLayout layout = tagLayoutFor(original, static_cast<uint8_t>(level));
        if (!layout.height || layout.width < 1) continue;
        for (int attempt = -1; attempt < 6 && !placed; ++attempt) {
          const int slot = attempt == -1 ? memory.slot : order[attempt];
          if (slot > 5 || (attempt >= 0 && slot == memory.slot)) continue;
          if (attempt >= 0 && memory.slot < 6 &&
              now - memory.switched_ms < 1250UL) continue;
          const LabelRect rect = tagSlotRect(items[d].x, items[d].y,
                                              layout.width, layout.height, slot);
          if (!rectFitsRoundScreen(rect)) continue;
          const int gap = slot == memory.slot ? 0 : 2;
          bool overlaps = false;
          for (size_t r = 0; r < reserved_count; ++r) {
            if (rectIntersectsGap(rect, reserved[r], gap)) {
              overlaps = true; break;
            }
          }
          if (overlaps) continue;
          for (size_t other_d = 0; other_d < count; ++other_d) {
            const float ox = items[other_d].x;
            const float oy = items[other_d].y;
            const LabelRect icon = {
                static_cast<int>(floorf(ox - icon_r)),
                static_cast<int>(floorf(oy - icon_r)),
                static_cast<int>(ceilf(ox + icon_r)),
                static_cast<int>(ceilf(oy + icon_r))};
            if (rectIntersectsGap(rect, icon, 0)) { overlaps = true; break; }
          }
          if (overlaps) continue;
          if (slot != memory.slot) {
            memory.slot = static_cast<uint8_t>(slot);
            memory.switched_ms = now;
          }
          if (memory.detail != level) {
            memory.detail = static_cast<uint8_t>(level);
            memory.detail_changed_ms = now;
          }
          drawAircraftTag(slot < 3 ? rect.x0 : rect.x1, rect.y0,
                           slot < 3, layout, custom_color, effective_brightness_q12);
          reserved[reserved_count++] = rect;
          memory.visible = true;
          placed = true;
        }
      }
      // A priority label must survive crowding. Use only a callsign / name
      // in the smaller native font if no collision-free label can be found.
      if (!placed && protected_label) {
        const TagLayout layout = tagLayoutFor(original, 4);
        if (!layout.height) continue;
        LabelRect candidate = {};
        int chosen_slot = -1;
        for (int attempt = 0; attempt < 6; ++attempt) {
          int slot = order[attempt];
          LabelRect r = tagSlotRect(items[d].x, items[d].y,
                                    layout.width, layout.height, slot);
          if (rectFitsRoundScreen(r)) { candidate = r; chosen_slot = slot; break; }
        }
        if (chosen_slot < 0) {
          // Physical screen may not fit a tag beside an aircraft at the rim.
          // Clamp a small callsign in the nearest quadrant as a last resort.
          const bool on_right = preferred_right;
          const int anchor = static_cast<int>(lroundf(items[d].x)) +
                             (on_right ? icon_r + 1 : -icon_r - 1);
          const int x = std::max(1, std::min(radar::kSize - layout.width - 1,
                                             on_right ? anchor : anchor - layout.width));
          const int y = std::max(1, std::min(radar::kSize - layout.height - 1,
                         static_cast<int>(lroundf(items[d].y)) - layout.height / 2));
          candidate = {x,y,x+layout.width,y+layout.height};
          chosen_slot = on_right ? 0 : 3;
        }
        drawAircraftTag(chosen_slot < 3 ? candidate.x0 : candidate.x1,
                         candidate.y0, chosen_slot < 3, layout, custom_color,
                         effective_brightness_q12);
        reserved[reserved_count++] = candidate;
        memory.slot = static_cast<uint8_t>(chosen_slot);
        if (memory.detail != 4) {
          memory.detail = 4;
          memory.detail_changed_ms = now;
        }
        memory.visible = true;
      }
    }
  }
}

// Time-faded aircraft trail history.
// Adaptive aircraft trails: fixed point budget, zoom-dependent sample spacing.
// 5/10/15 km: 5s, 25 km: 10s, 50 km: 15s, 75 km: 20s, 100 km: 30s.
// Positions remain independent of radar zoom; newer/older trails blend smoothly.
// Up to 64 x 32 x 6 bytes = 12 KiB of stored fixes; statically allocated.
constexpr size_t kTrailMaxTargets = services::adsb::kMaxAircraft;
constexpr size_t kTrailMaxSamples = 32;
struct TrailFix {
  int16_t east_10m;
  int16_t north_10m;
  uint16_t tick_1024;  // 1.024s clock ticks, wrap-safe within trail window
};
struct TrailHistory {
  char identity[16];
  uint32_t last_fetch_ms;
  uint32_t last_seen_ms;
  uint32_t last_sample_ms;
  uint8_t count;
  uint8_t next;
  uint16_t last_color;
  uint8_t last_hue;
  uint8_t last_shade;
  TrailFix fixes[kTrailMaxSamples];
};
// Cache one fractional palette position per fix at its real timestamp.
// Color lookup now happens AFTER spatial interpolation in the rasterizer.
TrailShadeLut buildTimedTrailShades(const TrailHistory& history, size_t visible,
                                    size_t capacity, uint32_t now_ms,
                                    uint32_t sample_interval_ms,
                                    uint16_t life_q12 = 4096) {
  TrailShadeLut result;
  const uint32_t reciprocal = ui::trail_fade::reciprocalIntervalQ16(sample_interval_ms);
  const uint8_t dark = ui::trails::startDarkeningPercent();
  const size_t first = static_cast<size_t>(history.count) - visible;
  for (size_t i = 0; i < visible; ++i) {
    const size_t idx = (history.next + kTrailMaxSamples - history.count + first + i)
                       % kTrailMaxSamples;
    const uint32_t elapsed_ms = ui::trail_fade::fixAgeMs(now_ms,
                                         history.fixes[idx].tick_1024);
    const uint32_t position_q8 = ui::trail_fade::agePositionQ8(
        elapsed_ms, sample_interval_ms, reciprocal, capacity);
    uint32_t shade_q8 = ui::trail_fade::shadeAtPositionQ8(
        history.last_shade, position_q8, capacity, dark);
    shade_q8 += (((255u << 8) - shade_q8) * (4096u - life_q12) + 2048u) >> 12;
    result.shades_q8[visible - 1 - i] = static_cast<uint16_t>(shade_q8);
  }
  return result;
}

static TrailHistory s_history[kTrailMaxTargets] = {};
static bool s_history_center_ready = false;
static double s_history_center_lat = 0.0;
static double s_history_center_lon = 0.0;

void formatTrailIdentity(const services::adsb::Aircraft& plane, char* dst,
                         size_t dst_len) {
  const char* raw = nullptr;
  char prefix = 'F';
  if (plane.hex[0]) { raw = plane.hex; prefix = 'H'; }
  else if (plane.registration[0]) { raw = plane.registration; prefix = 'R'; }
  else if (plane.callsign[0]) { raw = plane.callsign; }
  if (raw) snprintf(dst, dst_len, "%c:%s", prefix, raw);
  else dst[0] = '\0';
  for (size_t i = 0; dst[i] != '\0'; ++i) {
    if (dst[i] >= 'a' && dst[i] <= 'z') dst[i] -= ('a' - 'A');
  }
}

void resetTrailHistoryIfCenterMoved() {
  const double lat = services::location::lat();
  const double lon = services::location::lon();
  if (!s_history_center_ready ||
      fabs(lat - s_history_center_lat) > 0.000001 ||
      fabs(lon - s_history_center_lon) > 0.000001) {
    memset(s_history, 0, sizeof(s_history));
    memset(s_visuals, 0, sizeof(s_visuals));
    s_visual_last_fetch = 0;
    ui::turn_prediction::reset();
    s_history_center_lat = lat;
    s_history_center_lon = lon;
    s_history_center_ready = true;
  }
}

TrailHistory* trailFor(const char* identity, uint32_t now) {
  if (!identity[0]) return nullptr;
  TrailHistory* empty = nullptr;
  TrailHistory* oldest = &s_history[0];
  for (size_t i = 0; i < kTrailMaxTargets; ++i) {
    TrailHistory* t = &s_history[i];
    if (strcmp(t->identity, identity) == 0) return t;
    if (!empty && t->identity[0] == '\0') empty = t;
    if (static_cast<uint32_t>(now - t->last_seen_ms) >
        static_cast<uint32_t>(now - oldest->last_seen_ms)) oldest = t;
  }
  TrailHistory* t = empty ? empty : oldest;
  memset(t, 0, sizeof(*t));
  strncpy(t->identity, identity, sizeof(t->identity) - 1);
  return t;
}

int16_t trailCoord(float km) {
  const float tens = roundf(km * 100.0f);
  return static_cast<int16_t>(std::max(-32760.0f, std::min(32760.0f, tens)));
}

// Fetches still happen on their original ADS-B schedule; this ONLY throttles
// points stored in local trail history. No additional network operations.
uint32_t trailSampleIntervalMs(float outer_km) {
  if (outer_km <= 15.0f) return 5000u;
  if (outer_km <= 25.0f) return 10000u;
  if (outer_km <= 50.0f) return 15000u;
  if (outer_km <= 75.0f) return 20000u;
  return 30000u;
}

void rememberTrailFixes(const services::adsb::Aircraft* planes, size_t count,
                        unsigned long fetch_ms, uint32_t now) {
  if (fetch_ms == 0) return;
  resetTrailHistoryIfCenterMoved();
  if (ui::trails::points() == 0) return;
  const uint32_t interval_ms =
      trailSampleIntervalMs(radar::rangeCurrent().outer_km);
  for (size_t i = 0; i < count; ++i) {
    char id[16] = {};
    formatTrailIdentity(planes[i], id, sizeof(id));
    TrailHistory* t = trailFor(id, now);
    if (!t) continue;
    // Break the history if the target disappeared for a long time. Otherwise
    // a returning aircraft could draw a misleading line across the entire radar.
    if (t->count && static_cast<uint32_t>(now - t->last_seen_ms) > 120000u) {
      t->count = 0;
      t->next = 0;
    }
    t->last_seen_ms = now;
    if (t->last_fetch_ms == fetch_ms) continue;
    t->last_fetch_ms = fetch_ms;
    if (t->count && static_cast<uint32_t>(now - t->last_sample_ms) < interval_ms)
      continue;
    // Only retain genuine feed positions, never extrapolated display poses.
    // This also avoids filling the 32-point history with identical stale fixes.
    float east = 0.0f, north = 0.0f, dist = 0.0f;
    offsetKmFromCenter(planes[i].lat, planes[i].lon, &east, &north, &dist);
    const int16_t e = trailCoord(east);
    const int16_t n = trailCoord(north);
    if (t->count) {
      const TrailFix& prior = t->fixes[(t->next + kTrailMaxSamples - 1) % kTrailMaxSamples];
      if (prior.east_10m == e && prior.north_10m == n) continue;
    }
    t->last_sample_ms = now;
    t->fixes[t->next] = {e, n, static_cast<uint16_t>(now >> 10)};
    t->next = static_cast<uint8_t>((t->next + 1) % kTrailMaxSamples);
    if (t->count < kTrailMaxSamples) ++t->count;
  }
}

// Curve brightness now comes from the precomputed palette, not opacity.
// Keep unity intensity for antialiased stroke coverage and additive overlap.
float trailAlphaByAge(size_t, size_t, uint16_t = 0) { return 1.0f; }

float trailDistanceSq(float px, float py, float ax, float ay,
                      float vx, float vy, float inv_length_sq,
                      float* out_segment_t, bool rounded_caps = true) {
  float t = ((px - ax) * vx + (py - ay) * vy) * inv_length_sq;
  if (!rounded_caps && (t < 0.0f || t > 1.0f)) return 1000000.0f;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  if (out_segment_t) *out_segment_t = t;
  const float dx = px - (ax + t * vx);
  const float dy = py - (ay + t * vy);
  return dx * dx + dy * dy;
}

// One aircraft trail per group. 4 rows * 240 * RGB565 = 1,920 bytes,
// reused for every aircraft and orphan history. No full-size second buffer.
// Each group's segments MAX into this strip, then its final emission ADDS
// to the existing scene once. Different aircraft therefore still add light.
constexpr int kTrailStripRows = 4;
constexpr size_t kTrailGroupCapacity = kTrailMaxSamples + 5; // 31 history + 4 tip
struct TrailPrimitive {
  float ax, ay, bx, by;
  uint16_t c0, c1;  // RGB565 if solid, Q8 shade endpoints if gradient
  uint8_t hue;
  bool gradient;
};
static TrailPrimitive s_trail_group[kTrailGroupCapacity];
static size_t s_trail_group_count = 0;
static uint16_t s_trail_strip[radar::kSize * kTrailStripRows];
static bool s_collect_trail = false;
static bool s_render_trail_strip = false;
static int s_strip_y0 = 0, s_strip_y1 = -1;
static int s_strip_xmin = radar::kSize, s_strip_xmax = -1;

inline void putTrailEmission(int x, int y, uint16_t color, uint16_t intensity) {
  if (s_render_trail_strip) {
    if (y < s_strip_y0 || y > s_strip_y1) return;
    uint16_t& dst = s_trail_strip[(y - s_strip_y0) * radar::kSize + x];
    dst = ui::trail_compositing::max565(dst,
        ui::trail_compositing::scale565(color, intensity));
    if (dst) {
      if (x < s_strip_xmin) s_strip_xmin = x;
      if (x > s_strip_xmax) s_strip_xmax = x;
    }
  } else {
    // Only used outside grouped rendering (safe fallback for other callers).
    s_draw->drawPixel(x, y, addRgb565CoverageQ12(
        s_draw->readPixel(x, y), color, intensity));
  }
}

inline void collectTrailPrimitive(float ax, float ay, float bx, float by,
                                  uint16_t c0, uint16_t c1,
                                  uint8_t hue, bool gradient) {
  if (s_trail_group_count < kTrailGroupCapacity) {
    s_trail_group[s_trail_group_count++] = {ax, ay, bx, by, c0, c1, hue, gradient};
  }
  // Capacity always exceeds the maximum 32 stored points + 4 live sections.
}

// Fallback for projected endpoints too far offscreen for safe 32-bit Q4
// intermediates. This preserves the original rendering for those rare cases.
template <bool kColorGradient>
void drawTrailStrokeFloatFallback(float ax, float ay, float bx, float by,
                     float alpha_a, float alpha_b,
                     uint16_t color_start, uint16_t color_end,
                     uint8_t palette_hue = 0) {
  const float vx = bx - ax;
  const float vy = by - ay;
  const float length_sq = vx * vx + vy * vy;
  if (length_sq < 0.0001f) return;
  const float inv_length_sq = 1.0f / length_sq; // one division per segment
  const bool rounded_caps = true;  // Joins are always continuous.
  const bool show_dots = ui::display_options::trailDots();
  constexpr float kHalfWidth = 1.6f;
  constexpr float kCoreHalfWidth = 0.6f;
  constexpr float kBoundingPad = 2.70f;
  const int left = std::max(0, static_cast<int>(floorf(std::min(ax, bx) - kBoundingPad)));
  const int right = std::min(radar::kSize - 1, static_cast<int>(ceilf(std::max(ax, bx) + kBoundingPad)));
  const int top = std::max(s_render_trail_strip ? s_strip_y0 : 0,
      static_cast<int>(floorf(std::min(ay, by) - kBoundingPad)));
  const int bottom = std::min(s_render_trail_strip ? s_strip_y1 : radar::kSize - 1,
      static_cast<int>(ceilf(std::max(ay, by) + kBoundingPad)));
  if (left > right || top > bottom) return;
  const float ring_r = static_cast<float>(radar::kTrailDisplayRadiusPx) - 1.0f;
  const float ring_sq = ring_r * ring_r;
  const float coverage_radius_sq = kHalfWidth * kHalfWidth;
  const float core_radius_sq = kCoreHalfWidth * kCoreHalfWidth;
  (void)alpha_a;
  (void)alpha_b;
  for (int y = top; y <= bottom; ++y) {
    for (int x = left; x <= right; ++x) {
      const float rx = x + 0.5f - radar::kCenterX;
      const float ry = y + 0.5f - radar::kCenterY;
      if (rx * rx + ry * ry > ring_sq) continue;
      float center_t = 0.0f;
      const float d_sq = trailDistanceSq(x + 0.5f, y + 0.5f, ax, ay,
                                         vx, vy, inv_length_sq, &center_t,rounded_caps);
      const float reject_r = (show_dots ? 1.95f : kHalfWidth) + 0.7072f;
      if (d_sq >= reject_r * reject_r) continue;
      // 4x4 coverage for the 1.2px core and 3.2px shoulders.
      unsigned core_samples = 0, outer_samples = 0;
      for (int sy = 0; sy < 4; ++sy) {
        for (int sx = 0; sx < 4; ++sx) {
          const float sxp = x + (sx + 0.5f) * 0.25f;
          const float syp = y + (sy + 0.5f) * 0.25f;
          const float sample_sq = trailDistanceSq(sxp, syp, ax, ay,
              vx, vy, inv_length_sq, nullptr, rounded_caps);
          // Optional visible dots enlarge only the rounded sample caps,
          // never switch joins off when the checkbox is unchecked.
          const float along = ((sxp - ax) * vx + (syp - ay) * vy) * inv_length_sq;
          const bool dot_cap = show_dots && (along <= 0.0f || along >= 1.0f);
          if (sample_sq <= (dot_cap ? 1.95f * 1.95f : coverage_radius_sq)) {
            ++outer_samples;
            if (sample_sq <= core_radius_sq) ++core_samples;
          }
        }
      }
      const uint16_t intensity = ui::trail_beam::intensityQ12(
          core_samples, outer_samples);
      if (intensity < 61) continue;
      const uint16_t color = kColorGradient ?
          trailColorAtFractionQ12(palette_hue, color_start, color_end,
              static_cast<uint16_t>(lroundf(center_t * 4096.0f))) : color_start;
      putTrailEmission(x, y, color, intensity);
    }
  }
}


// Integer square root, rounded down. Called only for the three per-stroke
// distance thresholds; no floating-point sqrt remains in the normal renderer.
inline uint32_t trailIsqrt64(uint64_t n) {
  uint64_t bit = uint64_t{1} << 62;
  while (bit > n) bit >>= 2;
  uint64_t root = 0;
  while (bit != 0) {
    if (n >= root + bit) {
      n -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return static_cast<uint32_t>(root);
}

// Correct floor division for signed Q4 coordinates (including negative ones).
inline int32_t trailFloorQ4(int32_t q4) {
  return q4 >= 0 ? (q4 >> 4) : -(((-q4) + 15) >> 4);
}

// Q4 finite line segment for antialiased trails. Distance comparisons are
// performed with squared integer values; only the per-segment setup remains
// floating point. Preserve the existing rounded/endcap option and gradient.
struct TrailSegmentQ4 {
  int32_t ax, ay, dx, dy, len_sq;
};

// Squared radius is in Q8 units (Q4 coordinate squared). The precomputed
// cross threshold avoids most 64-bit multiplies on empty pixels.
inline bool trailSampleCovered(const TrailSegmentQ4& segment,
                               int32_t px, int32_t py, int32_t radius_sq,
                               int32_t cross_bound, bool rounded_caps,
                               int32_t* t_numerator) {
  const int32_t wx = px - segment.ax;
  const int32_t wy = py - segment.ay;
  const int32_t dot = wx * segment.dx + wy * segment.dy;
  if (!rounded_caps && (dot < 0 || dot > segment.len_sq)) return false;
  if (t_numerator) {
    *t_numerator = std::max<int32_t>(0,
        std::min<int32_t>(segment.len_sq, dot));
  }
  if (dot <= 0) return wx * wx + wy * wy <= radius_sq;
  if (dot >= segment.len_sq) {
    const int32_t ex = wx - segment.dx;
    const int32_t ey = wy - segment.dy;
    return ex * ex + ey * ey <= radius_sq;
  }
  const int32_t cross = wx * segment.dy - wy * segment.dx;
  const int32_t magnitude = std::abs(cross);
  if (magnitude + 1 < cross_bound) return true;
  if (magnitude > cross_bound + 1) return false;
  return static_cast<int64_t>(cross) * cross <=
      static_cast<int64_t>(radius_sq) * segment.len_sq;
}

// One segment projection and one distance per sample classify BOTH stroke
// widths. No second raster pass and no per-sample floats or divisions.
inline uint8_t trailBeamZone(const TrailSegmentQ4& segment,
                             int32_t px, int32_t py,
                             int32_t outer_cross, int32_t core_cross,
                             bool rounded_caps, bool show_dots) {
  const int32_t wx = px - segment.ax;
  const int32_t wy = py - segment.ay;
  const int32_t dot = wx * segment.dx + wy * segment.dy;
  if (!rounded_caps && (dot < 0 || dot > segment.len_sq)) return 0;
  int32_t dist_sq = 0;
  if (dot <= 0) {
    dist_sq = wx * wx + wy * wy;
    if (dist_sq <= ui::trail_beam::kCoreRadiusSqQ8) return 2;
    return dist_sq <= (show_dots ? 973 : ui::trail_beam::kOuterRadiusSqQ8) ? 1 : 0;
  }
  if (dot >= segment.len_sq) {
    const int32_t ex = wx - segment.dx;
    const int32_t ey = wy - segment.dy;
    dist_sq = ex * ex + ey * ey;
    if (dist_sq <= ui::trail_beam::kCoreRadiusSqQ8) return 2;
    return dist_sq <= (show_dots ? 973 : ui::trail_beam::kOuterRadiusSqQ8) ? 1 : 0;
  }
  const int32_t cross = wx * segment.dy - wy * segment.dx;
  const int32_t magnitude = std::abs(cross);
  if (magnitude + 1 < core_cross) return 2;
  if (magnitude > outer_cross + 1) return 0;
  const int64_t cross_sq = static_cast<int64_t>(cross) * cross;
  if (magnitude <= core_cross + 1 &&
      cross_sq <= static_cast<int64_t>(ui::trail_beam::kCoreRadiusSqQ8) *
          segment.len_sq) return 2;
  if (magnitude + 1 < outer_cross) return 1;
  return cross_sq <= static_cast<int64_t>(ui::trail_beam::kOuterRadiusSqQ8) *
      segment.len_sq ? 1 : 0;
}

template <bool kColorGradient>
void drawTrailStrokeImpl(float ax, float ay, float bx, float by,
                     float alpha_a, float alpha_b,
                     uint16_t color_start, uint16_t color_end,
                     uint8_t palette_hue = 0) {
  // All existing callers pass unity alpha (palette-based fading).
  // At ranges yielding off-screen coordinates beyond +/-400px, the Q4
  // cross products could overflow a signed 32-bit integer. Retain the
  // original safe float path for those unusual trail segments.
  if (!(std::isfinite(ax) && std::isfinite(ay) &&
        std::isfinite(bx) && std::isfinite(by))) return;
  if (std::max(std::max(fabsf(ax), fabsf(ay)),
               std::max(fabsf(bx), fabsf(by))) > 400.0f) {
    drawTrailStrokeFloatFallback<kColorGradient>(
        ax, ay, bx, by, alpha_a, alpha_b, color_start, color_end,
        palette_hue);
    return;
  }
  // Round the four geographic display coordinates ONCE per stroke. All
  // subsequent regular-path geometry, bounds, coverage, and color work is
  // integer/fixed-point. The float fallback above is exceptional only.
  const int32_t qax = rasterQ4(ax);
  const int32_t qay = rasterQ4(ay);
  const int32_t qbx = rasterQ4(bx);
  const int32_t qby = rasterQ4(by);
  TrailSegmentQ4 line = {qax, qay, qbx - qax, qby - qay, 0};
  line.len_sq = line.dx * line.dx + line.dy * line.dy;
  if (line.len_sq == 0) return;

  const bool rounded_caps = true;  // No gaps when trail dots are unchecked.
  const bool show_dots = ui::display_options::trailDots();
  // Cover the 1.6px radius (or 1.95px optional dots) plus AA margins.
  constexpr int32_t kPadQ4 = 44;
  const int left = std::max<int32_t>(0,
      trailFloorQ4(std::min(qax, qbx) - kPadQ4));
  const int right = std::min<int32_t>(radar::kSize - 1,
      trailFloorQ4(std::max(qax, qbx) + kPadQ4 + 15));
  const int top = std::max<int32_t>(s_render_trail_strip ? s_strip_y0 : 0,
      trailFloorQ4(std::min(qay, qby) - kPadQ4));
  const int bottom = std::min<int32_t>(s_render_trail_strip ? s_strip_y1 : radar::kSize - 1,
      trailFloorQ4(std::max(qay, qby) + kPadQ4 + 15));
  if (left > right || top > bottom) return;
  // Early-reject radius = (1.6px + half pixel diagonal)^2, in Q8.
  // Inner and outer stroke coverage use the same 16 samples.
  constexpr int32_t outer_sq = 1363;
  constexpr int32_t dot_outer_sq = 1808; // (1.95 + 0.707)^2 in Q8.
  const int32_t outer_cross = trailIsqrt64(
      static_cast<uint64_t>(show_dots ? dot_outer_sq : outer_sq) * line.len_sq);
  const int32_t cover_cross = trailIsqrt64(
      static_cast<uint64_t>(ui::trail_beam::kOuterRadiusSqQ8) * line.len_sq);
  const int32_t core_cross = trailIsqrt64(
      static_cast<uint64_t>(ui::trail_beam::kCoreRadiusSqQ8) * line.len_sq);
  const int32_t ring_radius_q4 =
      (radar::kTrailDisplayRadiusPx - 1) * kRasterOne;
  const int32_t ring_sq = ring_radius_q4 * ring_radius_q4;
  // All normal trail callers use unity endpoint alpha: fading is entirely
  // encoded in the palette. Edge coverage is the only remaining alpha.
  // Avoid a 64-bit endpoint-gradient division per segment and 16 further
  // divisions per antialiased pixel, important on the FPU-less ESP32-C3.
  (void)alpha_a;
  (void)alpha_b;
  const uint64_t color_recip_q48 = kColorGradient ?
      ui::trail_color_gradient::reciprocalQ48(
          static_cast<uint32_t>(line.len_sq)) : 0;

  for (int y = top; y <= bottom; ++y) {
    const int32_t py = y << kRasterShift;
    for (int x = left; x <= right; ++x) {
      const int32_t px = x << kRasterShift;
      const int32_t rx = px + 8 - (radar::kCenterX << kRasterShift);
      const int32_t ry = py + 8 - (radar::kCenterY << kRasterShift);
      if (rx * rx + ry * ry > ring_sq) continue;
      int32_t center_t = 0;
      if (!trailSampleCovered(line, px + 8, py + 8,
                              show_dots ? dot_outer_sq : outer_sq,
                              outer_cross, rounded_caps, &center_t)) continue;
      // One 4x4 subpixel pass: 100% core (1.2px), 50% shoulders
      // (to 3.2px total). Antialias both borders naturally.
      unsigned core_samples = 0, outer_samples = 0;
      for (int sy = 0; sy < 4; ++sy) {
        const int32_t qy = py + kRasterSample[sy];
        for (int sx = 0; sx < 4; ++sx) {
          const uint8_t zone = trailBeamZone(line,
              px + kRasterSample[sx], qy,
              cover_cross, core_cross, rounded_caps, show_dots);
          if (zone) {
            ++outer_samples;
            core_samples += (zone == 2);
          }
        }
      }
      const uint16_t intensity = ui::trail_beam::intensityQ12(
          core_samples, outer_samples);
      if (intensity < 61) continue; // Existing minimum visible contribution.
      // Interpolate continuous Q8 palette positions, then sample RGB888 and
      // convert once to RGB565 for additive blending. No early color rounding.
      const uint16_t color = kColorGradient ?
          trailColorAtFractionQ12(palette_hue, color_start, color_end,
              ui::trail_color_gradient::fractionQ12(center_t,
                  static_cast<uint32_t>(line.len_sq), color_recip_q48)) :
          color_start;
      putTrailEmission(x, y, color, intensity);
    }
  }
}

// Preserve the optimized rasterization path for normal trail segments.
void drawTrailStroke(float ax, float ay, float bx, float by,
                     float alpha_a, float alpha_b, uint16_t color) {
  if (s_collect_trail) {
    collectTrailPrimitive(ax, ay, bx, by, color, color, 0, false);
    return;
  }
  drawTrailStrokeImpl<false>(ax, ay, bx, by, alpha_a, alpha_b,
                             color, color);
}

// Every historical connection interpolates Q8 shade positions rather than
// already-rounded endpoint colors. Identical shades use the solid fast path.
void drawTrailStrokeGradient(float ax, float ay, float bx, float by,
                             float alpha_a, float alpha_b,
                             uint8_t hue, uint16_t shade_start_q8,
                             uint16_t shade_end_q8) {
  if (s_collect_trail) {
    collectTrailPrimitive(ax, ay, bx, by,
        shade_start_q8, shade_end_q8, hue, true);
    return;
  }
  if (shade_start_q8 == shade_end_q8) {
    drawTrailStroke(ax, ay, bx, by, alpha_a, alpha_b,
        trailColorAtFractionQ12(hue, shade_start_q8, shade_end_q8, 0));
    return;
  }
  drawTrailStrokeImpl<true>(ax, ay, bx, by, alpha_a, alpha_b,
                            shade_start_q8, shade_end_q8, hue);
}

// Complete one aircraft's MAX-composited emission, then add it to the
// existing RGB332 radar picture (including grid and previous aircraft).
// We only retain four rows at a time instead of an expensive second screen.
void beginTrailGroup() {
  s_trail_group_count = 0;
  s_collect_trail = true;
}
void flushTrailGroup() {
  s_collect_trail = false;
  if (!s_trail_group_count) return;
  int group_top = radar::kSize, group_bottom = -1;
  for (size_t i = 0; i < s_trail_group_count; ++i) {
    const TrailPrimitive& p = s_trail_group[i];
    if (!(std::isfinite(p.ay) && std::isfinite(p.by))) continue;
    const int lo = static_cast<int>(floorf(std::min(p.ay, p.by) - 2.7f));
    const int hi = static_cast<int>(ceilf(std::max(p.ay, p.by) + 2.7f));
    if (lo < group_top) group_top = lo;
    if (hi > group_bottom) group_bottom = hi;
  }
  group_top = std::max(0, group_top);
  group_bottom = std::min(radar::kSize - 1, group_bottom);
  for (int y0 = group_top; y0 <= group_bottom; y0 += kTrailStripRows) {
    s_strip_y0 = y0;
    s_strip_y1 = std::min(y0 + kTrailStripRows - 1, group_bottom);
    memset(s_trail_strip, 0, sizeof(s_trail_strip));
    s_strip_xmin = radar::kSize;
    s_strip_xmax = -1;
    s_render_trail_strip = true;
    for (size_t i = 0; i < s_trail_group_count; ++i) {
      const TrailPrimitive& p = s_trail_group[i];
      if (!std::isfinite(p.ay) || !std::isfinite(p.by) ||
          std::max(p.ay, p.by) + 2.7f < y0 ||
          std::min(p.ay, p.by) - 2.7f > s_strip_y1) continue;
      if (p.gradient) {
        drawTrailStrokeImpl<true>(p.ax, p.ay, p.bx, p.by, 1.0f, 1.0f,
                                  p.c0, p.c1, p.hue);
      } else {
        drawTrailStrokeImpl<false>(p.ax, p.ay, p.bx, p.by, 1.0f, 1.0f,
                                   p.c0, p.c1);
      }
    }
    s_render_trail_strip = false;
    if (s_strip_xmax < s_strip_xmin) continue;
    for (int y = y0; y <= s_strip_y1; ++y) {
      const uint16_t* row = s_trail_strip + (y - y0) * radar::kSize;
      for (int x = s_strip_xmin; x <= s_strip_xmax; ++x) {
        const uint16_t emission = row[x];
        if (!emission) continue;
        s_draw->drawPixel(x, y, ui::trail_compositing::add565(
            s_draw->readPixel(x, y), emission));
      }
    }
  }
  s_render_trail_strip = false;
  s_trail_group_count = 0;
}

void drawAircraftTrail(const services::adsb::Aircraft& plane,
                       const ui::turn_prediction::Pose& pose,
                       float head_x, float head_y,
                       services::aircraft_palette::Selection selection,
                       uint32_t now, float km_per_deg_lon,
                       bool connect_head = true) {
  const size_t max_visible = static_cast<size_t>(ui::trails::points());
  if (!max_visible) return;
  char id[16] = {};
  formatTrailIdentity(plane, id, sizeof(id));
  if (!id[0]) return;
  TrailHistory* t = nullptr;
  for (size_t i = 0; i < kTrailMaxTargets; ++i) {
    if (strcmp(s_history[i].identity, id) == 0) { t = &s_history[i]; break; }
  }
  if (!t || !t->count) return;
  t->last_color = color565ForRgb(selection.rgb);
  t->last_hue = selection.hue;
  t->last_shade = selection.shade;
  const float scale = static_cast<float>(radar::kGridOuterRadius) / radar::rangeCurrent().outer_km;
  const size_t visible = std::min(static_cast<size_t>(t->count), max_visible);
  // Use actual fix timestamps, NOT integer ages counted from the newest fix.
  // A fresh ADS-B fix no longer shifts all existing colors by one full step.
  const TrailShadeLut trail_shades = buildTimedTrailShades(
      *t, visible, max_visible, now,
      trailSampleIntervalMs(radar::rangeCurrent().outer_km));
  const uint16_t tip_shade_q8 = ui::trail_fade::shadeAtAgeQ8(
      selection.shade, 0, max_visible, 0,
      ui::trails::startDarkeningPercent());
  const uint16_t tip_color = trailColorAtFractionQ12(
      selection.hue, tip_shade_q8, tip_shade_q8, 0);
  // Guard old history when a plane returns to view after disappearing.
  const size_t first = static_cast<size_t>(t->count) - visible;
  float prev_x = 0.0f, prev_y = 0.0f, prev_a = 0.0f;
  bool have_previous = false;
  uint16_t previous_tick = 0;
  for (size_t i = 0; i < visible; ++i) {
    const size_t idx = (t->next + kTrailMaxSamples - t->count + first + i) % kTrailMaxSamples;
    const TrailFix& fix = t->fixes[idx];
    // Never connect fixes with a long unexplained absence between them.
    if (have_previous) {
      const uint16_t gap_ticks = static_cast<uint16_t>(fix.tick_1024 - previous_tick);
      if (static_cast<uint32_t>(gap_ticks) * 1024u > 120000u)
        have_previous = false;
    }
    const float alpha = trailAlphaByAge(
        visible - 1 - i, max_visible);
    const float x = radar::kCenterX + fix.east_10m * 0.01f * scale;
    const float y = radar::kCenterY - fix.north_10m * 0.01f * scale;
    if (have_previous) {
      // Interpolate every historical connection between the ages of its two
      // real ADS-B fixes. A flat color per segment caused visible brightness
      // steps, especially as the 32-entry palette advanced between updates.
      // The newest endpoint remains age zero, matching the live connection.
      const uint16_t older_shade = trail_shades.at(visible - i);
      const uint16_t newer_shade = trail_shades.at(visible - 1 - i);
      drawTrailStrokeGradient(prev_x, prev_y, x, y, prev_a, alpha,
                              selection.hue, older_shade, newer_shade);
    }
    prev_x = x; prev_y = y; prev_a = alpha;
    previous_tick = fix.tick_1024;
    have_previous = true;
  }
  // Connect newest ADS-B fix to current predicted head only if recent.
  if (have_previous && connect_head) {
    const uint16_t elapsed_ticks = static_cast<uint16_t>(
        static_cast<uint16_t>(now >> 10) - previous_tick);
    const uint32_t elapsed_ms = static_cast<uint32_t>(elapsed_ticks) * 1024u;
    if (elapsed_ms < 120000u) {
      const float tip_alpha = trailAlphaByAge(0, max_visible);
      // The history is made of actual ADS-B fixes. Connect it to the newest
      // genuine fix, then render a SHORT, temporary curved prediction to the
      // current rendered symbol. No guessed point is ever stored in TrailHistory.
      const float center_lon = static_cast<float>(services::location::lon());
      const float center_lat = static_cast<float>(services::location::lat());
      const float px_per_km = static_cast<float>(radar::kGridOuterRadius) /
                              radar::rangeCurrent().outer_km;
      const float actual_x = radar::kCenterX +
          (plane.lon - center_lon) * km_per_deg_lon * px_per_km;
      const float actual_y = radar::kCenterY -
          (plane.lat - center_lat) * kKmPerDeg * px_per_km;
      // Shade at the recorded fix already ages smoothly with time. Blend
      // that shade to the tip/aircraft shade over the live line, rather than
      // extending a solid bright line that snaps darker on the next sample.
      const uint16_t recorded_shade = trail_shades.at(0);
      drawTrailStrokeGradient(prev_x, prev_y, actual_x, actual_y,
                              prev_a, tip_alpha, selection.hue,
                              recorded_shade, tip_shade_q8);
      if (pose.turning && pose.age_s > 0.4f) {
        float from_x = actual_x, from_y = actual_y;
        // Three compact segments approximate the turn arc. Long histories
        // retain their normal 32 recorded samples; only this tip is virtual.
        constexpr int kTipSections = 3;
        for (int step = 1; step <= kTipSections; ++step) {
          float to_x = head_x, to_y = head_y;
          if (step != kTipSections) {
            float lat, lon;
            ui::turn_prediction::positionAt(plane, pose.track_rate_deg_s,
                pose.age_s * (static_cast<float>(step) / kTipSections),
                km_per_deg_lon, &lat, &lon);
            to_x = radar::kCenterX + (lon - center_lon) * km_per_deg_lon * px_per_km;
            to_y = radar::kCenterY - (lat - center_lat) * kKmPerDeg * px_per_km;
          }
          drawTrailStroke(from_x, from_y, to_x, to_y,
                          tip_alpha, tip_alpha, tip_color);
          from_x = to_x; from_y = to_y;
        }
      } else {
        drawTrailStroke(actual_x, actual_y, head_x, head_y,
                        tip_alpha, tip_alpha, tip_color);
      }
    }
  }
}


// When a plane disappears from the feed, retain its historical
// segments for 60 seconds. Fade the whole trail over that grace interval.
// Its icon and historical trail fade independently.
void drawOrphanTrails(const services::adsb::Aircraft* active, size_t n,
                      uint32_t now) {
  if (ui::trails::points() == 0) return;
  const size_t max_visible = static_cast<size_t>(ui::trails::points());
  const float scale = static_cast<float>(radar::kGridOuterRadius) /
                      radar::rangeCurrent().outer_km;
  for (size_t h = 0; h < kTrailMaxTargets; ++h) {
    const TrailHistory& history = s_history[h];
    if (!history.identity[0] || history.count < 2) continue;
    const uint32_t missing_ms = now - history.last_seen_ms;
    if (missing_ms >= 60000u) continue;
    bool still_present = false;
    for (size_t a = 0; a < n; ++a) {
      char id[16] = {};
      formatTrailIdentity(active[a], id, sizeof(id));
      if (strcmp(id, history.identity) == 0) { still_present = true; break; }
    }
    if (still_present) continue;
    beginTrailGroup();
    // Smooth quadratic fade using Q12 only (no per-orphan float division).
    const uint32_t life_q12 =
        ((60000u - missing_ms) * 4096u + 30000u) / 60000u;
    const uint16_t overall_fade_q12 = static_cast<uint16_t>(
        (life_q12 * life_q12 + 2048u) >> 12);
    const size_t visible = std::min(static_cast<size_t>(history.count), max_visible);
    const TrailShadeLut orphan_shades = buildTimedTrailShades(
        history, visible, max_visible, now,
        trailSampleIntervalMs(radar::rangeCurrent().outer_km),
        overall_fade_q12);
    const size_t first = static_cast<size_t>(history.count) - visible;
    float prev_x=0.0f, prev_y=0.0f, prev_alpha=0.0f;
    uint16_t previous_tick=0;
    bool have_previous=false;
    for (size_t i=0; i<visible; ++i) {
      const size_t idx = (history.next + kTrailMaxSamples - history.count + first + i)
                          % kTrailMaxSamples;
      const TrailFix& fix = history.fixes[idx];
      const float x = radar::kCenterX + fix.east_10m * 0.01f * scale;
      const float y = radar::kCenterY - fix.north_10m * 0.01f * scale;
      const float a = 1.0f; // Orphan fading is applied in the palette LUT.
      if (have_previous) {
        const uint16_t gap = static_cast<uint16_t>(fix.tick_1024 - previous_tick);
        if (static_cast<uint32_t>(gap) * 1024u <= 120000u) {
          // Fading orphan histories need the same continuous color gradient
          // as active histories, otherwise individual sections vanish in steps.
          drawTrailStrokeGradient(prev_x, prev_y, x, y, prev_alpha, a,
              history.last_hue, orphan_shades.at(visible - i),
              orphan_shades.at(visible - 1 - i));
        }
      }
      previous_tick = fix.tick_1024;
      prev_x=x; prev_y=y; prev_alpha=a;
      have_previous=true;
    }
    flushTrailGroup();
  }
}

bool hasUnexpiredTrail(uint32_t now) {
  if (ui::trails::points() == 0) return false;
  for (const TrailHistory& history : s_history) {
    if (history.identity[0] && history.count > 1 &&
        static_cast<uint32_t>(now - history.last_seen_ms) < 60000u) {
      return true;
    }
  }
  return false;
}

void drawAircraftLayer() {
  const uint32_t aircraft_started = micros();
  initLabelMetrics();

  // Snapshot under the adsb lock (the fetch may run on another thread).
  static services::adsb::Aircraft planes[kVisualCapacity];
  unsigned long base_ms = 0;
  const size_t n = services::adsb::snapshotAircraft(
      planes, services::adsb::kMaxAircraft, &base_ms);
  const uint32_t trail_now = millis();
  rememberTrailFixes(planes, n, base_ms, trail_now);
  updateVisualCache(planes, n, static_cast<uint32_t>(base_ms));

  // Draw normal aircraft first and a bounded set of fading ghosts afterward.
  // Ghosts neither dead-reckon nor add trail fixes. Their last pose is frozen.
  static uint16_t visual_q12[kVisualCapacity];
  static ui::turn_prediction::Pose poses[kVisualCapacity];
  static AircraftDrawItem items[kVisualCapacity];
  static BeyondDotDrawItem dots[kVisualCapacity];
  for (size_t i = 0; i < n; ++i) {
    char id[16] = {};
    formatTrailIdentity(planes[i], id, sizeof(id));
    const VisualEntry* v = visualFor(id);
    visual_q12[i] = v ? visualIntensity(*v, trail_now) : 4096u;
  }
  size_t total = n;
  for (const VisualEntry& v : s_visuals) {
    if (total >= kVisualCapacity) break;
    if (!v.identity[0] || v.present) continue;
    const uint16_t q12 = visualIntensity(v, trail_now);
    if (!q12) continue;
    planes[total] = v.last_aircraft;
    poses[total] = v.last_pose;
    visual_q12[total] = q12;
    ++total;
  }
  const float km_per_deg_lon = kKmPerDeg * cosf(
      static_cast<float>(services::location::lat()) * kDegToRad);
  size_t draw_count = 0;
  size_t dot_count = 0;

  for (size_t i = 0; i < total; ++i) {
    // Active targets predict motion; ghosts use the last frozen display pose.
    if (i < n) {
      char identity[16] = {};
      formatTrailIdentity(planes[i], identity, sizeof(identity));
      poses[i] = ui::turn_prediction::predict(identity, planes[i], base_ms,
                                               trail_now, km_per_deg_lon);
      VisualEntry* v = visualFor(identity);
      if (v) v->last_pose = poses[i];
    }
    const float lat = poses[i].lat;
    const float lon = poses[i].lon;

    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

    if (isInsideOuterRingKm(dist_km)) {
      // Reuse the offsets calculated above. The old path called cosf and
      // sqrtf again for exactly these same coordinates.
      const float px_per_km = static_cast<float>(radar::kGridOuterRadius) /
                              radar::rangeCurrent().outer_km;
      const float x = radar::kCenterX + dx_km * px_per_km;
      const float y = radar::kCenterY - dy_km * px_per_km;
      items[draw_count].index = i;
      items[draw_count].x = x;
      items[draw_count].y = y;
      items[draw_count].dist_sq = distSqFromCenter(
          static_cast<int>(lroundf(x)), static_cast<int>(lroundf(y)));
      ++draw_count;
      continue;
    }

    if (dist_km < 0.01f) continue;
    // Reuse the already calculated bearing components and distance.
    const float rim = static_cast<float>(radar::kCenterX -
                                         radar::kBeyondRingScreenMarginPx);
    const float unit_scale = rim / dist_km;
    const float dot_x = radar::kCenterX + dx_km * unit_scale;
    const float dot_y = radar::kCenterY - dy_km * unit_scale;
    dots[dot_count].index = i;
    dots[dot_count].x = dot_x;
    dots[dot_count].y = dot_y;
    dots[dot_count].dist_sq = static_cast<int>(dist_km * 100.0f);
    dots[dot_count].radius = beyondDotRadiusKm(dist_km);
    ++dot_count;
  }

  sortDrawItemsFarFirst(items, draw_count);
  s_stage_timing.projection_us = micros() - aircraft_started;
  const uint32_t trails_started = micros();
  drawOrphanTrails(planes, n, trail_now);
  // Historical trails remain underneath labels and symbols; elapsed time
  // controls fading, and the maximum history is still 32 points per target.
  if (ui::trails::points() > 0) {
    for (size_t d = 0; d < draw_count; ++d) {
      const size_t i = items[d].index;
      if (i >= n) continue;  // no new trail points for a fading ghost
      beginTrailGroup();
      drawAircraftTrail(planes[i], poses[i], items[d].x, items[d].y,
                        aircraftColorSelection(planes[i]),
                        trail_now, km_per_deg_lon);
      flushTrailGroup();
    }
    // The edge dot stays locked to bearing and must NEVER be used as a trail
    // endpoint: distant targets can move along the rim without tracing it.
    // However the LIVE trail tip must not disappear as soon as the icon
    // becomes a dot. Continue to the aircraft's true projected position,
    // even if offscreen. The trail rasterizer clips it to the circular rim.
    // This prevents a missing live section and the next ADS-B fix suddenly
    // painting an entire length of trail at the edge.
    const float px_per_km = static_cast<float>(radar::kGridOuterRadius) /
                            radar::rangeCurrent().outer_km;
    for (size_t d = 0; d < dot_count; ++d) {
      const size_t i = dots[d].index;
      if (i >= n) continue;  // no live trail tip for an edge-dot ghost
      const float x = radar::kCenterX +
          (poses[i].lon - services::location::lon()) * km_per_deg_lon * px_per_km;
      const float y = radar::kCenterY -
          (poses[i].lat - services::location::lat()) * kKmPerDeg * px_per_km;
      beginTrailGroup();
      drawAircraftTrail(planes[i], poses[i], x, y,
                        aircraftColorSelection(planes[i]),
                        trail_now, km_per_deg_lon);
      flushTrailGroup();
    }
  }
  s_stage_timing.trails_us = micros() - trails_started;
  const uint32_t dots_started = micros();
  sortBeyondDotsFarFirst(dots, dot_count);
  for (size_t d = 0; d < dot_count; ++d) {
    const size_t i = dots[d].index;
    const uint16_t default_color = planes[i].isMilitary
        ? radar::kColorMilitaryAircraft : radar::kColorAircraft;
    if (visual_q12[i]) {
      drawBeyondRingDot(dots[d].x, dots[d].y, dots[d].radius,
                        watchedAircraftColor(planes[i], default_color),
                        visual_q12[i]);
    }
  }
  s_stage_timing.dots_us = micros() - dots_started;
  const uint32_t labels_started = micros();
  placeAircraftTags(items, draw_count, planes, n);
  s_stage_timing.labels_us = micros() - labels_started;
  const uint32_t icons_started = micros();

  // Aircraft symbols are drawn last, above labels and trails, every full frame.
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    const float x = items[d].x;
    const float y = items[d].y;
    // V9 replaces the forward speed vector with a dim historical trail.
    const uint16_t default_color = planes[i].isMilitary ? radar::kColorMilitaryAircraft
                                                        : radar::kColorAircraft;
    if (!visual_q12[i]) continue;
    const uint16_t symbol_color = watchedAircraftColor(planes[i], default_color);
    if (planes[i].isHelicopter)
      drawHelicopterRing(x, y, symbol_color, visual_q12[i]);
    drawHeadingTriangle(x, y, poses[i].nose_deg, symbol_color, visual_q12[i]);
  }
  s_stage_timing.icons_us = micros() - icons_started;
  s_stage_timing.aircraft_us = micros() - aircraft_started;
}

void applyCardinalStyle() {
  if (s_cardinal_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_cardinal_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_cardinal_gfx);
  }
}

void applyScaleStyle() {
  if (s_scale_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_scale_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_scale_gfx);
  }
}

void drawCardinalLabel(const char* text, int x, int y, textdatum_t datum) {
  applyCardinalStyle();
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

// Reuse the native compact bitmap font from aircraft secondary text.
// An opaque black mask keeps radar rings and spokes from passing
// runs through the range label. Native V13 6x9 pixel text remains crisp.
void drawScaleLabelWithBackground(const char* text, int x, int y) {
  const int w = plane_label_font::width(text, false);
  if (w <= 0) return;
  constexpr int kFontHeight = 9;
  constexpr int kPadX = 2;
  constexpr int kPadY = 2;
  const int left = x - w;
  const int top = y - 4; // same text baseline as V14
  s_draw->fillRect(left - kPadX, top - kPadY,
                   w + kPadX * 2, kFontHeight + kPadY * 2,
                   static_cast<uint16_t>(0x0000));
  // This is the PREVIOUS V14 second ring from the outside (64%).
  plane_label_font::draw(*s_draw, left, top, text, s_scale_label_color, false);
}

// Draw one circle per ring,
// making each line two pixels thick and sometimes appear doubled.
void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r > 0) s_draw->drawCircle(cx, cy, r, color);
}

// Radial intensity steps, brightest nearest the centre.
// Subdued ring shading, darker outward.
// Colors chosen for the RGB332 framebuffer.
// Inner two rings intentionally share the same RGB332 shade (112).
// Keep the outer ring dimmer than the inner rings.
void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, s_ring_colors[std::min(i - 1, 3)]);
  }
}

void drawCrosshairs(int cx, int cy, int radius, uint16_t color) {
  s_draw->drawWideLine(cx, cy - radius, cx, cy + radius,
                       radar::kGridStrokeHalfWidth, color);
  s_draw->drawWideLine(cx - radius, cy, cx + radius, cy,
                       radar::kGridStrokeHalfWidth, color);
}

void drawCenterDot(int cx, int cy) {
  s_draw->fillSmoothCircle(cx, cy, radar::kCenterDotRadius, radar::kColorCenterDot);
}

// Native single-stroke pixel glyphs aligned to the radar axes.
void drawCardinalLabels() {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;
  constexpr int cell_w = 7;
  constexpr int cell_h = 11;
  constexpr int margin = 3;
  const int x_mid = cx - cell_w / 2;
  const int y_mid = cy - cell_h / 2;
  plane_label_font::draw(*s_draw, x_mid, margin, "N", radar::kColorCenter, true);
  plane_label_font::draw(*s_draw, x_mid, edge - margin - cell_h + 1,
                         "S", radar::kColorCenter, true);
  plane_label_font::draw(*s_draw, margin, y_mid, "W", radar::kColorCenter, true);
  plane_label_font::draw(*s_draw, edge - margin - cell_w + 1, y_mid,
                         "E", radar::kColorCenter, true);
}

int scaleLabelAnchorX(int cx, int outer_radius) {
  return cx + outer_radius - radar::kScaleGapFromOuterRing;
}

void drawScaleLabel(int cx, int cy, int outer_radius) {
  char scale_label[12];
  radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
  drawScaleLabelWithBackground(scale_label,
                               scaleLabelAnchorX(cx, outer_radius), cy);
}

// Dynamic fallback grid used if an embedded flash asset is invalid.
// Normal operation copies the approved antialiased background directly.
template <typename Gfx>
void drawGridBaseOriginal(Gfx& gfx) {
  const DrawScope scope(gfx);
  gfx.fillScreen(radar::kColorBackground);
  drawRings(radar::kCenterX, radar::kCenterY, radar::kGridOuterRadius);
  drawCrosshairs(radar::kCenterX, radar::kCenterY,
                 radar::kGridOuterRadius, s_cross_color);
}

// Range, location and UI options change these graphics. They must NOT be
// frozen into the flash image. They draw over either base layer exactly as
// in the original implementation; the runway geometry cache stays active.
template <typename Gfx>
void drawGridDynamicOverlay(Gfx& gfx) {
  const DrawScope scope(gfx);
  const uint32_t runway_started = micros();
  runway::drawLargeAirportRunways(gfx);
  s_stage_timing.runways_us = micros() - runway_started;
  drawCenterDot(radar::kCenterX, radar::kCenterY);
  if (radar::backgroundPattern() != radar::BackgroundPattern::SuperMinimal) {
    drawCardinalLabels();
  }
  drawScaleLabel(radar::kCenterX, radar::kCenterY,
                 radar::kGridOuterRadius);
  gfx.setTextDatum(textdatum_t::top_left);
}

// The embedded antialiased bitmap is trusted after a size check.
// Avoid runtime redraws or full-image comparisons.
bool validateFlashGrid(LGFX_Sprite& gfx) {
  if (s_flash_grid_fallback >= 0) return s_flash_grid_fallback == 0;
  s_flash_grid_fallback = 1;
  const FlashGridAsset asset = currentFlashGridAsset();
  const std::ptrdiff_t embedded_size = asset.end - asset.start;
  if (embedded_size != static_cast<std::ptrdiff_t>(kFlashGridBytes) ||
      gfx.bufferLength() < kFlashGridBytes || gfx.getBuffer() == nullptr) {
    Serial.printf("radar flash grid: invalid bitmap/sprite lengths (%ld, %lu); using original grid\r\n",
                  static_cast<long>(embedded_size),
                  static_cast<unsigned long>(gfx.bufferLength()));
    return false;
  }
  s_flash_grid_fallback = 0;
  Serial.println("radar flash grid: anti-aliased image active");
  return true;
}

bool drawFlashGrid(LGFX_Sprite& gfx) {
  if (!validateFlashGrid(gfx)) return false;
  const uint32_t copy_started = micros();
  const FlashGridAsset asset = currentFlashGridAsset();
  // Map one of eight stored brightness levels into RGB332, directly into
  // the existing framebuffer. NO second bitmap and no extra 240x240 buffer.
  const uint8_t* src = asset.start;
  uint8_t* dst = reinterpret_cast<uint8_t*>(gfx.getBuffer());
  for (size_t i = 0; i < kFlashGridBytes; ++i) {
    dst[i] = s_bg_palette332[(src[i] >> 2) & 7];
  }
  s_stage_timing.flash_copy_us = micros() - copy_started;
  drawGridDynamicOverlay(gfx);
  return true;
}

template <typename Gfx>
void drawStaticGrid(Gfx& gfx) {
  initLabelMetrics();
  drawGridBaseOriginal(gfx);
  drawGridDynamicOverlay(gfx);
}

bool ensureFrameSprite() {
  if (s_frame_ready) return true;
  // A failed allocation is not retried each frame.
  if (s_frame_attempted) return false;
  s_frame_attempted = true;
  s_frame.setColorDepth(lgfx::color_depth_t::rgb332_1Byte);
  if (!s_frame.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: one-frame RGB332 allocation failed; drawing directly");
    return false;
  }
  s_frame_ready = true;
  Serial.printf("radar: single RGB332 framebuffer ready; free heap %lu bytes\r\n",
                static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}

void drawWifiResetCountdown() {
  const uint8_t seconds = wifiResetCountdownSeconds();
  if (!seconds) return;
  const int x = 33, y = 108;
  s_draw->fillRect(x - 5, y - 4, radar::kSize - 2 * x + 10, 24,
                   radar::kColorBackground);
  char message[27];
  snprintf(message, sizeof(message), "WIFI RESET IN %us",
           static_cast<unsigned>(seconds));
  plane_label_font::draw(*s_draw, x, y, message,
                         tft.color565(255, 255, 0), false);
}

// Full-frame rendering, scheduled at 2..6.25 Hz by range in main.
// One RGB332 framebuffer; redraw the scene, trails, labels, edge dots and
// aircraft icons together so no black restoration holes or ghost symbols.
// Web requests can temporarily defer this call without blocking Wi-Fi/ADS-B.
void renderFrame(bool force) {
  const uint32_t started = micros();
  const uint32_t now = millis();
  const bool scene_dynamic = services::adsb::aircraftCount() != 0 ||
                             hasUnexpiredTrail(now) || hasFadingGhost(now);
  const bool countdown_active = wifiResetCountdownSeconds() != 0;
  const bool countdown_changed = countdown_active != s_last_countdown_active;
  s_last_countdown_active = countdown_active;

  // After the final aircraft and trail have disappeared, draw one last clean
  // frame, then leave the unchanged radar idle until something changes.
  if (!force && !s_scene_dirty && !scene_dynamic &&
      !s_last_scene_dynamic && !countdown_active && !countdown_changed) {
    s_last_frame_us = 0;
    return;
  }

  s_stage_timing = {};
  s_stage_timing.cpu_mhz_frame_start = static_cast<uint16_t>(getCpuFrequencyMhz());
  const uint32_t compose_start = micros();
  if (ensureFrameSprite()) {
    const uint32_t grid_started = micros();
    initLabelMetrics();
    if (!drawFlashGrid(s_frame)) {
      drawStaticGrid(s_frame);  // Safe fallback only for invalid flash assets.
    }
    s_stage_timing.grid_us = micros() - grid_started;
    {
      const DrawScope scope(s_frame);
      drawAircraftLayer();   // Trails -> edge dots -> labels -> symbols.
      drawWifiResetCountdown();
    }
    s_last_scene_us = micros() - compose_start;
    const uint16_t transfer_cpu_start = static_cast<uint16_t>(getCpuFrequencyMhz());
    const uint32_t transfer_started = micros();
    s_frame.pushSprite(0, 0);
    s_stage_timing.transfer_us = micros() - transfer_started;
    const uint16_t transfer_cpu_end = static_cast<uint16_t>(getCpuFrequencyMhz());
    // Normal 240x240 transfer is ~14 ms. Count the ~28 ms outliers even
    // when Status polling happens after the frame has passed.
    if (s_stage_timing.transfer_us >= 22000u) {
      ++s_spi_slow_diag.count;
      s_spi_slow_diag.cpu_mhz_start = transfer_cpu_start;
      s_spi_slow_diag.cpu_mhz_end = transfer_cpu_end;
      s_spi_slow_diag.last_transfer_us = s_stage_timing.transfer_us;
    }
  } else {
    // Last-resort direct-rendering fallback if the one sprite cannot allocate.
    const DrawScope scope(tft);
    const uint32_t grid_started = micros();
    drawStaticGrid(tft);
    s_stage_timing.grid_us = micros() - grid_started;
    drawAircraftLayer();
    drawWifiResetCountdown();
    s_last_scene_us = micros() - compose_start;
  }

  s_scene_dirty = false;
  s_last_scene_dynamic = scene_dynamic;
  s_last_frame_us = micros() - started;
  s_stage_timing.cpu_mhz_frame_end = static_cast<uint16_t>(getCpuFrequencyMhz());
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();
  renderFrame(true);
}

void radarDisplayRefreshAircraft() {
  initPalette();
  renderFrame(false);
}

void radarDisplayInvalidateSlowLayer() { s_scene_dirty = true; }

void radarDisplayThemeChanged() {
  s_palette_ready = false;
  s_scene_dirty = true;
}
uint32_t radarDisplayLastRenderUs() { return s_last_frame_us; }
uint32_t radarDisplayLastSlowRenderUs() { return s_last_scene_us; }
uint8_t radarDisplayFramebufferCount() { return s_frame_ready ? 1 : 0; }
RadarFrameTimings radarDisplayFrameTimings() { return s_stage_timing; }
RadarSpiSlowDiag radarDisplaySpiSlowDiag() { return s_spi_slow_diag; }
bool radarDisplayFlashGridActive() { return s_flash_grid_fallback == 0; }

}  // namespace ui

// Single-column aircraft label layout.
