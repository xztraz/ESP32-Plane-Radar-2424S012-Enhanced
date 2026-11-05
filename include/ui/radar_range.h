#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::radar {

/**
 * Range presets (label on ring 3 = ¾ of outer radius).
 *
 * Recommended for ADS-B on a 1.28″ display:
 *   5 km  — pattern / very local (airfield vicinity)
 *  10 km  — default; neighborhood spotting
 *  15 km  — wider local area
 *  25 km  — metro / regional picture
 *  50/75/100 km — broad air traffic picture with progressively lower FPS
 *
 * Outer radius (for aircraft math) is ring-3 distance ÷ 0.75.
 */
struct RangePreset {
  /** Distance shown on ring 3 (¾ of outer radius), always stored in km. */
  float ring3_km;
  float outer_km;
  uint16_t redraw_interval_ms;  // Range-dependent full-frame cadence.
};

constexpr float kRing3ToOuterKm = 4.0f / 3.0f;

// Full-frame refresh rate decreases with range: aircraft cover fewer screen
// pixels per second when viewed farther away. These exact integer intervals
// avoid fractional-millisecond scheduling and reduce CPU load at 75/100 km.
constexpr RangePreset kRangePresets[] = {
    {5.0f,   5.0f * kRing3ToOuterKm, 160},  // 6.25 FPS
    {10.0f, 10.0f * kRing3ToOuterKm, 160},  // 6.25 FPS
    {15.0f, 15.0f * kRing3ToOuterKm, 200},  // 5 FPS
    {25.0f, 25.0f * kRing3ToOuterKm, 200},  // 5 FPS
    {50.0f, 50.0f * kRing3ToOuterKm, 250},  // 4 FPS
    {75.0f, 75.0f * kRing3ToOuterKm, 320},  // 3.125 FPS
    {100.0f, 100.0f * kRing3ToOuterKm, 500},  // 2 FPS
};

constexpr size_t kRangePresetCount =
    sizeof(kRangePresets) / sizeof(kRangePresets[0]);


enum class BackgroundTheme : uint8_t {
  Green = 0,
  Red = 1,
  Amber = 2,
  Cyan = 3,
  Violet = 4,
  White = 5,
};

enum class BackgroundPattern : uint8_t {
  Original = 0,
  ClassicPpi = 1,
  Minimal = 2,
  PolarGrid = 3,
  DashedRings = 4,
  SuperMinimal = 5,
};

/** Load saved range and distance units from flash. Call once after boot. */
void rangeInit();
/** Cycle preset and save to flash. */
void rangeNext();
/** Select range via web settings, without requiring repeated button taps. */
void rangeSetIndex(uint8_t index);
const RangePreset& rangeCurrent();
uint8_t rangeIndex();
/** Exact integer milliseconds between full-frame redraw starts for current range. */
uint16_t redrawIntervalMs();
/** ADSB fetch radius (km): scaled to screen edge so beyond-ring dots have data. */
float fetchRadiusKm();

bool useMiles();
bool showRunways();
BackgroundTheme backgroundTheme();
BackgroundPattern backgroundPattern();
void saveBackgroundTheme(uint8_t theme_index);
void saveBackgroundPattern(uint8_t pattern_index);
/** WiFi portal checkbox: "T" = miles, otherwise km. */
void saveMilesFromPortal(const char* checkbox_value);
void saveRunwaysFromPortal(const char* checkbox_value);
void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles);
void formatCurrentRing3Label(char* buf, size_t len);
/** Reset distance units to km (e.g. with WiFi credential wipe). */
void unitsReset();

}  // namespace ui::radar
