#include "ui/trail_color_gradient.h"
#include "services/aircraft_palette.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
  using ui::trail_color_gradient::interpolateShadeQ8;
  for (uint32_t start = 0; start <= 65280; start += 197) {
    for (uint32_t end = 0; end <= 65280; end += 509) {
      assert(interpolateShadeQ8(start, end, 0) == start);
      assert(interpolateShadeQ8(start, end, 4096) == end);
      assert(interpolateShadeQ8(start, end, 6000) == end);
      for (uint32_t t=1;t<4096;t+=53) {
        const uint16_t shade = interpolateShadeQ8(start, end, t);
        const uint32_t expected = (start*(4096-t)+end*t+2048)>>12;
        assert(shade == expected);
        assert(shade >= std::min(start,end) && shade <= std::max(start,end));
      }
    }
  }
  // Fractional changes survive interpolation even when full palette indices
  // are unchanged: positions can pass through the color thresholds in time.
  assert(interpolateShadeQ8(100u*256u + 1u, 100u*256u + 255u, 2048u)
      == 100u*256u + 128u);
  // Final palette samples match the original high-precision path at endpoints.
  for (unsigned hue=0;hue<256;hue+=17) {
    for (unsigned shade=0;shade<=255;shade+=7) {
      auto a = services::aircraft_palette::rgb888SmoothAtQ8(
          static_cast<uint8_t>(hue), static_cast<uint16_t>(shade*256));
      auto b = services::aircraft_palette::rgb888SmoothAtQ8(
          static_cast<uint8_t>(hue), interpolateShadeQ8(shade*256,shade*256,2048));
      assert(a==b);
    }
  }
  puts("V17.27 fractional palette spatial interpolation: OK");
}
