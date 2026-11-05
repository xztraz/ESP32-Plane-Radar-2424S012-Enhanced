#pragma once
#include <cstdint>

// 3.2 px total width with 1.2 px bright core. Q4 coordinates,
// squared Q8 radius thresholds preserve the requested fractional widths.
// Both core and shoulder edges use the SAME 4x4 subpixel grid.
namespace ui::trail_beam {
constexpr int32_t kOuterRadiusQ4 = 26;  // ceil(1.6 * 16); bounds only
constexpr int32_t kCoreRadiusQ4 = 10;   // ceil(0.6 * 16); bounds only
constexpr int32_t kOuterRadiusSqQ8 = 655; // round(1.6^2 * 256)
constexpr int32_t kCoreRadiusSqQ8 = 92;  // round(0.6^2 * 256)
constexpr uint16_t kFullQ12 = 4096;
constexpr uint16_t kShoulderQ12 = 2048; // 50%

// Sample intensity: full center, half-brightness shoulder, outside zero.
// The same 16 samples antialias both the core/shoulder transition and rim.
inline uint16_t intensityQ12(unsigned core_samples, unsigned outer_samples) {
  if (outer_samples > 16u || core_samples > outer_samples) return 0;
  return static_cast<uint16_t>((core_samples * kFullQ12 +
      (outer_samples - core_samples) * kShoulderQ12 + 8u) >> 4);
}
} // namespace ui::trail_beam
