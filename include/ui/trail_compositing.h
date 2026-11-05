#pragma once
#include <cstdint>

// Each aircraft's segments are composited by per-channel MAX into
// a 4-row scratch strip. Only after the whole trail has been merged is it
// saturating-added ONCE to the RGB332 radar scene. No double exposure at
// joints, while distinct aircraft trails and the background still add.
namespace ui::trail_compositing {
inline uint16_t scale565(uint16_t color, uint16_t coverage_q12) {
  if (coverage_q12 > 4096u) coverage_q12 = 4096u;
  const uint32_t r = (((color >> 11) & 31u) * coverage_q12 + 2048u) >> 12;
  const uint32_t g = (((color >> 5) & 63u) * coverage_q12 + 2048u) >> 12;
  const uint32_t b = ((color & 31u) * coverage_q12 + 2048u) >> 12;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}
inline uint16_t max565(uint16_t a, uint16_t b) {
  const uint16_t r = ((a >> 11) & 31u) > ((b >> 11) & 31u) ? (a & 0xf800u) : (b & 0xf800u);
  const uint16_t g = ((a >> 5) & 63u) > ((b >> 5) & 63u) ? (a & 0x07e0u) : (b & 0x07e0u);
  const uint16_t bl = (a & 31u) > (b & 31u) ? (a & 0x001fu) : (b & 0x001fu);
  return static_cast<uint16_t>(r | g | bl);
}
inline uint16_t add565(uint16_t bg, uint16_t emission) {
  const uint16_t r = static_cast<uint16_t>((((bg >> 11) & 31u) + ((emission >> 11) & 31u)) > 31u ? 31u : (((bg >> 11) & 31u) + ((emission >> 11) & 31u)));
  const uint16_t g = static_cast<uint16_t>((((bg >> 5) & 63u) + ((emission >> 5) & 63u)) > 63u ? 63u : (((bg >> 5) & 63u) + ((emission >> 5) & 63u)));
  const uint16_t b = static_cast<uint16_t>(((bg & 31u) + (emission & 31u)) > 31u ? 31u : ((bg & 31u) + (emission & 31u)));
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}
inline uint16_t addCoverage565(uint16_t bg, uint16_t color, uint16_t coverage_q12) {
  return add565(bg, scale565(color, coverage_q12));
}
}  // namespace ui::trail_compositing
