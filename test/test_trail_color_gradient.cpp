#include "ui/trail_color_gradient.h"
#include "ui/trail_fade.h"
#include "services/aircraft_palette.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
using namespace ui::trail_color_gradient;

static uint16_t rgb565(uint32_t rgb) {
  const uint8_t r = static_cast<uint8_t>(rgb >> 16);
  const uint8_t g = static_cast<uint8_t>(rgb >> 8);
  const uint8_t b = static_cast<uint8_t>(rgb);
  return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}
int main() {
  // Endpoints always exact for every 16-bit RGB565 channel combination sampled.
  for (uint32_t a = 0; a < 65536; a += 113) {
    for (uint32_t b = 0; b < 65536; b += 131) {
      assert(blend565Q12(a, b, 0) == a);
      assert(blend565Q12(a, b, kOneQ12) == b);
      assert(blend565Q12(a, b, 55000) == b);
      assert(blend565Q12(a, a, 2048) == a);
      for (uint16_t t : {uint16_t(1), uint16_t(1024), uint16_t(2048), uint16_t(3072), uint16_t(4095)}) {
        uint16_t blended = blend565Q12(a, b, t);
        for (unsigned shift : {unsigned(0), unsigned(5), unsigned(11)}) {
          const unsigned mask = shift == 5 ? 63 : 31;
          const unsigned x = (a >> shift) & mask;
          const unsigned y = (b >> shift) & mask;
          const unsigned v = (blended >> shift) & mask;
          assert(v >= std::min(x,y) && v <= std::max(x,y));
        }
      }
    }
  }
  // Q4 length squared fits into int32_t for the guarded renderer. No per-pixel
  // divisions; tolerance <= 1 Q12 level at all relevant segment lengths.
  for (uint32_t len : {1u,2u,10u,256u,2048u,65536u,1048576u,10000000u,200000000u}) {
    uint64_t recip = reciprocalQ48(len);
    assert(fractionQ12(0, len, recip) == 0);
    assert(fractionQ12(len, len, recip) == 4096);
    assert(fractionQ12(-1, len, recip) == 0);
    assert(fractionQ12(len + 1, len, recip) == 4096);
    for (unsigned step = 0; step <= 100; ++step) {
      uint32_t pos = static_cast<uint32_t>((static_cast<uint64_t>(len) * step) / 100);
      uint16_t got = fractionQ12(pos, len, recip);
      uint16_t expected = static_cast<uint16_t>((static_cast<uint64_t>(pos) * 4096u + len/2u) / len);
      assert(abs(int(got)-int(expected)) <= 2);
    }
  }
  // The color at either end of the live section, immediately before a new
  // real fix is stored, must match the newest history segment endpoints
  // immediately after insertion, including a white head with colored tail.
  for (unsigned hue=0;hue<256;++hue) {
    for (unsigned shade=0;shade<256;shade+=7) {
      for (unsigned dark=0;dark<=100;dark+=5) {
        const uint8_t previous_fix_shade = ui::trail_fade::shadeAtAge(
            shade, 0, 32, 256, dark);
        const uint8_t historical_shade = ui::trail_fade::shadeAtAge(
            shade, 1, 32, 0, dark);
        assert(previous_fix_shade == historical_shade);
        const uint8_t head_shade = ui::trail_fade::shadeAtAge(shade, 0, 32, 0, dark);
        const uint16_t live_from = rgb565(services::aircraft_palette::at(hue, previous_fix_shade).rgb);
        const uint16_t history_from = rgb565(services::aircraft_palette::at(hue, historical_shade).rgb);
        const uint16_t live_to = rgb565(services::aircraft_palette::at(hue, head_shade).rgb);
        const uint16_t history_to = rgb565(services::aircraft_palette::at(hue, head_shade).rgb);
        assert(live_from == history_from && live_to == history_to);
        assert(ui::trail_fade::alphaQ12(0,32,256) == ui::trail_fade::alphaQ12(1,32,0));
      }
    }
  }
  std::puts("V17.21 RGB565 endpoint interpolation / handoff continuity: OK");
}
