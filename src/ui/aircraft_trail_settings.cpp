#include "ui/aircraft_trail_settings.h"
#include "ui/trail_fade.h"
#include "ui/trail_dots.h"
#include <Preferences.h>
#include <cerrno>
#include <cstdlib>

namespace ui::trails {
namespace {
constexpr char kPrefs[] = "airtrail";
// Separate key from the retired duration format: seconds must not be interpreted as points.
constexpr char kKey[] = "points";
constexpr char kDarkeningKey[] = "startdark";
constexpr char kCurveKey[] = "fadecurve";
constexpr char kDotSpacingKey[] = "dotspace";
constexpr char kDotDiameterKey[] = "dotdiam2";
constexpr uint16_t kDefaultPoints = 24;
constexpr uint16_t kMaxPoints = 32;
constexpr uint8_t kDefaultDarkening = 15;  // New unset-profile default, percentage.
uint16_t s_points = kDefaultPoints;
uint8_t s_start_darkening = kDefaultDarkening;
constexpr uint8_t kDefaultCurve = 2;  // Phosphor afterglow.
uint8_t s_curve = kDefaultCurve;
uint8_t s_dot_spacing = ui::trail_dots::kDefaultSpacing;
uint8_t s_dot_diameter = ui::trail_dots::kDefaultDiameterHalfPx;
bool s_initialized = false;
}
uint16_t points() {
  if (!s_initialized) {
    s_initialized = true;
    Preferences p;
    if (p.begin(kPrefs, true)) {
      const uint16_t saved = p.getUShort(kKey, kDefaultPoints);
      // Older V16/V17 profiles may have saved 33..64. Clamp them rather
      // than silently reverting to the default of 24 points.
      s_points = saved > kMaxPoints ? kMaxPoints : saved;
      const uint8_t d = p.getUChar(kDarkeningKey, kDefaultDarkening);
      s_start_darkening = d <= 100 ? d : kDefaultDarkening;
      const uint8_t stored_curve = p.getUChar(kCurveKey, kDefaultCurve);
      s_curve = stored_curve < ui::trail_fade::kCurveCount ? stored_curve : kDefaultCurve;
      const uint8_t stored_spacing = p.getUChar(kDotSpacingKey, ui::trail_dots::kDefaultSpacing);
      s_dot_spacing = stored_spacing >= ui::trail_dots::kMinSpacing &&
                      stored_spacing <= ui::trail_dots::kMaxSpacing ? stored_spacing :
                      ui::trail_dots::kDefaultSpacing;
      const uint8_t stored_diameter = p.getUChar(kDotDiameterKey, ui::trail_dots::kDefaultDiameterHalfPx);
      s_dot_diameter = stored_diameter >= ui::trail_dots::kMinDiameterHalfPx &&
                       stored_diameter <= ui::trail_dots::kMaxDiameterHalfPx ? stored_diameter :
                       ui::trail_dots::kDefaultDiameterHalfPx;
      p.end();
    }
    // This only runs once at startup. Switching profiles later also calls
    // selectCurve() immediately, from the web-save handler (main loop).
    ui::trail_fade::selectCurve(s_curve);
  }
  return s_points;
}
uint8_t startDarkeningPercent() {
  (void)points();  // Shared one-time load of trail settings.
  return s_start_darkening;
}
uint8_t fadeCurve() {
  (void)points();
  return s_curve;
}
uint8_t dotSpacing() {
  (void)points();
  return s_dot_spacing;
}
uint8_t dotDiameterHalfPx() {
  (void)points();
  return s_dot_diameter;
}
bool saveFromPortal(const char* text) {
  if (!text || !*text) return false;
  errno = 0;
  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  if (errno || !end || *end != '\0' || value < 0 || value > kMaxPoints)
    return false;
  (void)points();
  if (s_points == static_cast<uint16_t>(value)) return true;
  Preferences p;
  if (!p.begin(kPrefs, false)) return false;
  const size_t written = p.putUShort(kKey, static_cast<uint16_t>(value));
  p.end();
  if (written != sizeof(uint16_t)) return false;
  s_points = static_cast<uint16_t>(value);
  return true;
}
bool saveStartDarkeningPercent(const char* text) {
  if (!text || !*text) return false;
  errno = 0;
  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  if (errno || !end || *end != '\0' || value < 0 || value > 100)
    return false;
  (void)points();
  if (s_start_darkening == static_cast<uint8_t>(value)) return true;
  Preferences p;
  if (!p.begin(kPrefs, false)) return false;
  const size_t written = p.putUChar(kDarkeningKey, static_cast<uint8_t>(value));
  p.end();
  if (written != sizeof(uint8_t)) return false;
  s_start_darkening = static_cast<uint8_t>(value);
  return true;
}
bool saveFadeCurve(const char* text) {
  if (!text || *text < '0' || *text > '9') return false;
  errno = 0;
  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  if (errno || !end || *end != '\0' ||
      value < 0 || value >= ui::trail_fade::kCurveCount) return false;
  (void)points();
  if (s_curve == static_cast<uint8_t>(value)) return true;
  Preferences p;
  if (!p.begin(kPrefs, false)) return false;
  const size_t written = p.putUChar(kCurveKey, static_cast<uint8_t>(value));
  p.end();
  if (written != sizeof(uint8_t)) return false;
  s_curve = static_cast<uint8_t>(value);
  // The web handler and the framebuffer redraw share the main loop, so this
  // atomic 64-byte table switch never runs in the middle of a frame.
  ui::trail_fade::selectCurve(s_curve);
  return true;
}
// Both preferences are single bytes; the existing settings namespace and
// framebuffer are reused. These functions run in the web/main loop only.
static bool saveDotByte(const char* text, uint8_t min_value, uint8_t max_value,
                        const char* key, uint8_t& dest) {
  if (!text || !*text) return false;
  errno = 0;
  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  if (errno || !end || *end != '\0' || value < min_value || value > max_value)
    return false;
  (void)points();
  if (dest == static_cast<uint8_t>(value)) return true;
  Preferences p;
  if (!p.begin(kPrefs, false)) return false;
  const size_t written = p.putUChar(key, static_cast<uint8_t>(value));
  p.end();
  if (written != sizeof(uint8_t)) return false;
  dest = static_cast<uint8_t>(value);
  return true;
}
bool saveDotSpacing(const char* text) {
  return saveDotByte(text, ui::trail_dots::kMinSpacing,
                     ui::trail_dots::kMaxSpacing, kDotSpacingKey, s_dot_spacing);
}
bool saveDotDiameterHalfPx(const char* text) {
  return saveDotByte(text, ui::trail_dots::kMinDiameterHalfPx,
                     ui::trail_dots::kMaxDiameterHalfPx, kDotDiameterKey,
                     s_dot_diameter);
}
} // namespace ui::trails
