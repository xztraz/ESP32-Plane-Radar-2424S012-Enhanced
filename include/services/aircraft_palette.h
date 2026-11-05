#pragma once

#include <cstddef>
#include <cstdint>

// The browser's 2D picker stores BOTH hue (horizontal position) and shade
// (vertical position). A white aircraft can therefore retain a green/blue/red
// trail even though its RGB332 icon color is the same 0xFF pixel.
namespace services::aircraft_palette {
struct Selection {
  uint32_t rgb = 0;
  uint8_t hue = 0;
  uint8_t shade = 102;
};

inline uint8_t clamp8(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Piecewise HSV-like color field: white at shade=0, vivid hue at shade=102,
// black at shade=255. Integer-only and explicitly quantized to RGB332.
// These formulas are duplicated exactly in the browser JavaScript.
// High-precision version of the *same* white -> hue -> black palette.
// Trail colors must not pass through RGB332 before interpolation: doing so
// creates large discrete steps where the entire segment changes color.
// The selected aircraft icon continues using the exact original RGB332 color.
inline uint32_t rgb888SmoothAtQ8(uint8_t hue, uint16_t shade_q8) {
  const uint16_t h = static_cast<uint16_t>(hue) * 6u;
  const uint8_t sector = h >> 8;
  const uint8_t f = h & 255u;
  uint8_t r = 0, g = 0, b = 0;
  switch (sector) {
    case 0: r = 255; g = f; break;
    case 1: r = 255 - f; g = 255; break;
    case 2: g = 255; b = f; break;
    case 3: g = 255 - f; b = 255; break;
    case 4: r = f; b = 255; break;
    default: r = 255; b = 255 - f; break;
  }
  auto adjust = [shade_q8](uint8_t channel) -> uint8_t {
    if (shade_q8 <= (102u << 8)) {
      const uint32_t diff = 255u - channel;
      return static_cast<uint8_t>(255u -
          (diff * shade_q8 + (102u << 7)) / (102u << 8));
    }
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(channel) * ((255u << 8) - shade_q8) +
         (153u << 7)) / (153u << 8));
  };
  return (static_cast<uint32_t>(adjust(r)) << 16) |
         (static_cast<uint32_t>(adjust(g)) << 8) | adjust(b);
}
inline uint32_t rgb888SmoothAt(uint8_t hue, uint8_t shade) {
  return rgb888SmoothAtQ8(hue, static_cast<uint16_t>(shade) << 8);
}

inline uint8_t rgb332At(uint8_t hue, uint8_t shade) {
  const uint16_t h = static_cast<uint16_t>(hue) * 6;
  const uint8_t sector = h >> 8;
  const uint8_t f = h & 255;
  uint8_t r = 0, g = 0, b = 0;
  switch (sector) {
    case 0: r = 255; g = f; break;
    case 1: r = 255 - f; g = 255; break;
    case 2: g = 255; b = f; break;
    case 3: g = 255 - f; b = 255; break;
    case 4: r = f; b = 255; break;
    default: r = 255; b = 255 - f; break;
  }
  auto adjust = [shade](uint8_t channel) -> uint8_t {
    if (shade <= 102) {
      const unsigned diff = 255u - channel;
      return static_cast<uint8_t>(255u - (diff * shade + 51u) / 102u);
    }
    return static_cast<uint8_t>((static_cast<unsigned>(channel) * (255u - shade) + 76u) / 153u);
  };
  r = adjust(r); g = adjust(g); b = adjust(b);
  return static_cast<uint8_t>((((static_cast<unsigned>(r) * 7 + 127) / 255) << 5) |
                              (((static_cast<unsigned>(g) * 7 + 127) / 255) << 2) |
                              ((static_cast<unsigned>(b) * 3 + 127) / 255));
}
inline uint32_t rgb888From332(uint8_t p) {
  const uint8_t r = static_cast<uint8_t>((((p >> 5) & 7) * 255 + 3) / 7);
  const uint8_t g = static_cast<uint8_t>((((p >> 2) & 7) * 255 + 3) / 7);
  const uint8_t b = static_cast<uint8_t>((p & 3) * 85);
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}
inline Selection at(uint8_t hue, uint8_t shade) {
  Selection result;
  result.rgb = rgb888From332(rgb332At(hue, shade));
  result.hue = hue;
  result.shade = shade;
  return result;
}
inline int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
inline bool decode(const char* source, size_t len, Selection* result) {
  if (!source || !result || (len != 6 && len != 10)) return false;
  uint32_t rgb = 0;
  for (size_t i = 0; i < 6; ++i) {
    const int digit = nibble(source[i]);
    if (digit < 0) return false;
    rgb = (rgb << 4) | static_cast<uint32_t>(digit);
  }
  if (len == 10) {
    uint16_t coords = 0;
    for (size_t i = 6; i < 10; ++i) {
      const int digit = nibble(source[i]);
      if (digit < 0) return false;
      coords = (coords << 4) | digit;
    }
    Selection selected = at(static_cast<uint8_t>(coords >> 8),
                            static_cast<uint8_t>(coords));
    // Do not allow mismatched RGB/hue/shade triples to be stored.
    if (selected.rgb != rgb) return false;
    *result = selected;
    return true;
  }
  // Older color tokens: retain rules and settings where possible, but derive a
  // suitable hue for old data. The user does not require identical old colors.
  const int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
  const int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
  const int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
  const int delta = hi - lo;
  int hue = 0;
  if (delta > 0) {
    if (hi == r) hue = ((g - b) * 256 / delta + 6 * 256) % (6 * 256);
    else if (hi == g) hue = (2 * 256) + (b - r) * 256 / delta;
    else hue = (4 * 256) + (r - g) * 256 / delta;
  }
  uint8_t y = 102;
  if (delta == 0) y = clamp8((255 - hi) * 102 / 255);
  else if (hi < 255) y = clamp8(102 + (255 - hi) * 153 / 255);
  else if (lo > 0) y = clamp8((lo * 102 + 127) / 255);
  *result = at(static_cast<uint8_t>((hue + 3) / 6), y);
  return true;
}
// The global start-darkening slider is a percentage of the remaining shade
// distance toward black. 0 keeps the newest trail exactly at the aircraft's
// RGB332 hue/shade; 100 starts it black. Older trail fixes still descend along
// that same hue column. No allocations or floating point in this conversion.
inline uint8_t trailShade(uint8_t selected_shade, uint8_t age,
                         uint8_t capacity, uint8_t darkening_percent = 0) {
  const int gap = 255 - static_cast<int>(selected_shade);
  const int start = static_cast<int>(selected_shade) +
      (gap * static_cast<int>(darkening_percent) + 50) / 100;
  const int step = capacity > 1 ? (static_cast<int>(age) * (255 - start)) /
                                          (static_cast<int>(capacity) - 1) : 0;
  return clamp8(start + step);
}
}  // namespace services::aircraft_palette
