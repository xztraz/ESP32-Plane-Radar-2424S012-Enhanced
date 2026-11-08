#pragma once
#include <cstdint>

// Lightweight, fixed-point trail sample markers. There is no second position
// history and no floating-point work in the marker placement or AA rasterizer.
namespace ui::trail_dots {
constexpr uint8_t kMinSpacing = 1;
constexpr uint8_t kMaxSpacing = 8;
constexpr uint8_t kDefaultSpacing = 3;
// Diameter stored in half-pixel units: 4 = 2.0 px, 9 = 4.5 px, 14 = 7.0 px.
constexpr uint8_t kMinDiameterHalfPx = 4;
constexpr uint8_t kMaxDiameterHalfPx = 14;
constexpr uint8_t kDefaultDiameterHalfPx = 9;

inline uint8_t nextPhase(uint8_t phase, uint8_t spacing) {
  if (spacing < kMinSpacing || spacing > kMaxSpacing) return 0;
  return static_cast<uint8_t>((phase + 1u) % spacing);
}
// Phase belongs to the NEWEST recorded fix, not a rotating buffer index.
// Age zero is the newest fix; modulo spacing keeps all historical dots locked
// to their original fixes, including after the 32-entry ring buffer wraps.
inline bool hasDot(uint8_t newest_phase, uint8_t spacing, uint8_t age) {
  if (spacing < kMinSpacing || spacing > kMaxSpacing) return false;
  return (static_cast<uint8_t>((newest_phase + spacing -
                  (age % spacing)) % spacing) == 0);
}

// The stored coordinate is in 10 m units. The displayed third ring is at
// three-quarters of the outer grid's mapped world distance. Calculate pixel
// displacement in Q4 with symmetric rounding, using ONLY integer arithmetic:
// pxQ4 = fix10m * gridOuterPx * 3 / (25 * ring3Km).
inline int32_t offsetQ4(int16_t fix10m, uint16_t ring3_km,
                        uint16_t grid_outer_px) {
  if (!ring3_km) return 0;
  const int32_t numerator = static_cast<int32_t>(fix10m) *
                            grid_outer_px * 3;
  const int32_t denominator = static_cast<int32_t>(ring3_km) * 25;
  return numerator >= 0
       ? (numerator + denominator / 2) / denominator
       : -((-numerator + denominator / 2) / denominator);
}

// Circle's AA shoulder is 70% emission; core is 100%. Already faded RGB565
// color is added (with saturation) to the existing RGB332 framebuffer.
inline uint16_t coverageQ12(unsigned core_samples, unsigned outer_samples) {
  if (outer_samples > 16 || core_samples > outer_samples) return 0;
  return static_cast<uint16_t>((core_samples * 4096u +
      (outer_samples - core_samples) * 2867u + 8u) >> 4);
}
} // namespace ui::trail_dots
