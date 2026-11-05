#pragma once
#include <cstddef>
#include <cstdint>

// Time-interpolated trail ages and four selectable palette curves.
// No dynamic allocations, no floating-point calculations per pixel.
// Only one curve lookup per historic trail endpoint. The fixed-point
// antialiasing rasterizer still uses geometric subpixel coverage.
namespace ui { namespace trail_fade {
constexpr uint16_t kFullAlphaQ12 = 4096;
constexpr size_t kCurveEntries = 32;
constexpr uint8_t kCurveCount = 4;
// Curve IDs: 0=refined Smooth S, 1=Linear,
//            2=Phosphor afterglow (default), 3=Late Fade.
// Four 64-byte read-only tables live in flash; selecting a curve copies one
// to a 64-byte RAM table between frames. No heap allocation is involved.
void selectCurve(uint8_t curve);
uint8_t selectedCurve();
const uint16_t* activeCurveQ12();

// 0..256 represents fractional progression since the most recent historic
// ADS-B fix. At one complete sample interval, the previous fix is exactly
// one point older; insertion of the next fix is therefore visually continuous.
inline uint16_t phaseQ8(uint32_t now_ms, uint32_t last_sample_ms,
                        uint32_t interval_ms) {
  if (interval_ms == 0) return 0;
  const uint32_t elapsed = now_ms - last_sample_ms; // wrap-safe uint32_t
  if (elapsed >= interval_ms) return 256;
  return static_cast<uint16_t>((elapsed * 256u) / interval_ms);
}

// Evaluate the curve at an absolute fractional age measured in units of one
// sampling interval. It is not tied to a stored segment index: boundaries can
// travel through segments, even when actual ADS-B fixes are irregularly spaced.
inline uint16_t alphaAtPositionQ12(uint32_t age_q8, size_t capacity) {
  if (capacity == 0) return 0;
  if (capacity == 1) return kFullAlphaQ12;
  const uint32_t total_q8 = static_cast<uint32_t>(capacity - 1) * 256u;
  if (age_q8 >= total_q8) return 0;
  const uint32_t curve_q8 = (age_q8 * (kCurveEntries - 1)) /
      static_cast<uint32_t>(capacity - 1);
  const size_t index = curve_q8 >> 8;
  const uint32_t frac = curve_q8 & 255u;
  const uint16_t* curve = activeCurveQ12();
  return static_cast<uint16_t>((curve[index] * (256u - frac) +
                                curve[index + 1] * frac + 128u) >> 8);
}

// Convert the 1.024s timestamp saved with each fix to its elapsed age. Time
// wraps safely within the <18-hour history horizon. No floating point.
inline uint32_t fixAgeMs(uint32_t now_ms, uint16_t fix_tick_1024) {
  return static_cast<uint32_t>(static_cast<uint16_t>(
      (now_ms >> 10) - fix_tick_1024)) * 1024u + (now_ms & 1023u);
}

// Reciprocal is calculated once per aircraft. Multiplication, clamp and shift
// replace one division per endpoint. Caller uses at most 32 intervals, keeping
// products safely within uint32_t even at the 30-second sample interval.
inline uint32_t reciprocalIntervalQ16(uint32_t interval_ms) {
  return interval_ms ? ((256u << 16) + interval_ms / 2u) / interval_ms : 0;
}
inline uint32_t agePositionQ8(uint32_t age_ms, uint32_t interval_ms,
                              uint32_t reciprocal_q16, size_t capacity) {
  if (!interval_ms || capacity < 2) return 0;
  const uint32_t limit_ms = static_cast<uint32_t>(capacity - 1) * interval_ms;
  if (age_ms >= limit_ms) return static_cast<uint32_t>(capacity - 1) * 256u;
  return (age_ms * reciprocal_q16 + 32768u) >> 16;
}

inline uint16_t alphaQ12(size_t age, size_t capacity, uint16_t phase_q8) {
  const uint32_t phase = phase_q8 > 256 ? 256u : phase_q8;
  return alphaAtPositionQ12(static_cast<uint32_t>(age) * 256u + phase, capacity);
}

// Use the chosen curve for *palette travel*, not opacity.
// The icon stays at selected_shade. Darkening only shifts the starting
// shade of its trail down the existing white -> hue -> black column.
// Keep fractional ADS-B age interpolation to avoid popping between fixes.
// Fractional palette position (Q8: 0..65280) instead of quantizing the
// palette index to a whole 0..255 value before sampling its RGB channels.
// This preserves the fractional age progression that was previously visible
// without bringing age alpha back.
inline uint16_t shadeAtPositionQ8(uint8_t selected_shade,
                                  uint32_t position_q8, size_t capacity,
                                  uint8_t darkening_percent) {
  const uint32_t remaining = 255u - selected_shade;
  const uint32_t start_q8 = (static_cast<uint32_t>(selected_shade) << 8) +
      ((remaining << 8) * darkening_percent + 50u) / 100u;
  const uint32_t distance_q12 = 4096u - alphaAtPositionQ12(position_q8, capacity);
  return static_cast<uint16_t>(start_q8 +
      (((255u << 8) - start_q8) * distance_q12 + 2048u) / 4096u);
}

inline uint16_t shadeAtAgeQ8(uint8_t selected_shade, size_t age,
                             size_t capacity, uint16_t phase_q8,
                             uint8_t darkening_percent) {
  const uint32_t phase = phase_q8 > 256 ? 256u : phase_q8;
  return shadeAtPositionQ8(selected_shade,
      static_cast<uint32_t>(age) * 256u + phase, capacity, darkening_percent);
}

// Retain the whole-index API for legacy callers and existing compatibility tests.
inline uint8_t shadeAtAge(uint8_t selected_shade, size_t age,
                          size_t capacity, uint16_t phase_q8,
                          uint8_t darkening_percent) {
  return static_cast<uint8_t>((shadeAtAgeQ8(selected_shade, age, capacity,
                              phase_q8, darkening_percent) + 128u) >> 8);
}

}} // namespace ui::trail_fade
