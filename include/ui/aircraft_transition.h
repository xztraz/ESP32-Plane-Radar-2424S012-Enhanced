#pragma once
#include <cstdint>

namespace ui::aircraft_transition {
constexpr uint32_t kFadeInMs = 2000u;
constexpr uint32_t kFadeOutMs = 12000u;
constexpr uint16_t kFull = 4096u;
// Integer smoothstep: monotonic and symmetric, no FPU or alpha buffer.
inline uint16_t ease(uint32_t elapsed_ms, uint32_t duration_ms) {
  if (elapsed_ms >= duration_ms) return kFull;
  const uint32_t t = (elapsed_ms * 4096u) / duration_ms;
  const uint32_t curve = static_cast<uint32_t>(
      ((static_cast<uint64_t>(t) * t * (12288u - 2u * t)) + (1ull << 23)) >> 24);
  return static_cast<uint16_t>(curve > kFull ? kFull : curve);
}
inline uint16_t intensity(bool present, uint32_t elapsed_ms,
                          uint16_t initial_q12,
                          const uint16_t* selected_trail_curve = nullptr) {
  if (initial_q12 > kFull) initial_q12 = kFull;
  if (present) {
    const uint32_t e = ease(elapsed_ms, kFadeInMs);
    return static_cast<uint16_t>(initial_q12 +
        ((static_cast<uint32_t>(kFull - initial_q12) * e + 2048u) >> 12));
  }
  uint32_t remaining = kFull - ease(elapsed_ms, kFadeOutMs);
  if (selected_trail_curve != nullptr && elapsed_ms < kFadeOutMs) {
    // Same selected brightness curve as the trails, but based on twelve seconds
    // of elapsed disappearance time rather than trail point age.
    const uint32_t pos_q8 = (elapsed_ms * (31u * 256u)) / kFadeOutMs;
    const uint32_t idx = pos_q8 >> 8;
    const uint32_t frac = pos_q8 & 255u;
    remaining = (static_cast<uint32_t>(selected_trail_curve[idx]) * (256u - frac) +
                 static_cast<uint32_t>(selected_trail_curve[idx + 1u]) * frac + 128u) >> 8;
  }
  return static_cast<uint16_t>((static_cast<uint32_t>(initial_q12) *
                                remaining + 2048u) >> 12);
}
inline uint16_t multiply(uint16_t a, uint16_t b) {
  return static_cast<uint16_t>((static_cast<uint32_t>(a) * b + 2048u) >> 12);
}
}  // namespace ui::aircraft_transition
