#pragma once
#include <cstdint>

// RGB565 color interpolation for every historical and live trail segment.
// Identical endpoint colors still use the solid-color fast path.
// No extra framebuffer or heap objects.
namespace ui { namespace trail_color_gradient {
constexpr uint16_t kOneQ12 = 4096;

// Interpolate the palette coordinate while it still has 8 fractional bits.
// Do not quantize the two endpoints to RGB565 before interpolation.
inline uint16_t interpolateShadeQ8(uint16_t first, uint16_t last,
                                   uint16_t fraction_q12) {
  const uint32_t t = fraction_q12 > kOneQ12 ? kOneQ12 : fraction_q12;
  return static_cast<uint16_t>((static_cast<uint32_t>(first) * (kOneQ12 - t) +
      static_cast<uint32_t>(last) * t + 2048u) >> 12);
}

inline uint16_t blend565Q12(uint16_t first, uint16_t last,
                           uint16_t fraction_q12) {
  const uint32_t t = fraction_q12 > kOneQ12 ? kOneQ12 : fraction_q12;
  const uint32_t inv = kOneQ12 - t;
  const uint32_t r = (((first >> 11) & 31u) * inv +
                      ((last >> 11) & 31u) * t + 2048u) >> 12;
  const uint32_t g = (((first >> 5) & 63u) * inv +
                      ((last >> 5) & 63u) * t + 2048u) >> 12;
  const uint32_t b = ((first & 31u) * inv +
                      (last & 31u) * t + 2048u) >> 12;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

// Prepare one reciprocal per segment. For a covered pixel the fractional
// position then needs only multiplication/shift (not integer division).
// 2^48 leaves enough precision for long offscreen Q4 lines, and the
// product with a clamped t_numerator still fits in uint64_t.
inline uint64_t reciprocalQ48(uint32_t len_sq) {
  return len_sq ? ((1ULL << 48) + len_sq / 2u) / len_sq : 0;
}
inline uint16_t fractionQ12(int32_t t_numerator, uint32_t len_sq,
                            uint64_t reciprocal_q48) {
  if (t_numerator <= 0 || len_sq == 0) return 0;
  if (static_cast<uint32_t>(t_numerator) >= len_sq) return kOneQ12;
  const uint64_t q12 = (static_cast<uint64_t>(t_numerator) * reciprocal_q48 +
                        (1ULL << 35)) >> 36;
  return static_cast<uint16_t>(q12 > kOneQ12 ? kOneQ12 : q12);
}
}} // namespace ui::trail_color_gradient
