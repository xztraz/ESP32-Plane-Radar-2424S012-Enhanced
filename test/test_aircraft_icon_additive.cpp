#include "ui/trail_compositing.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
using namespace ui::trail_compositing;

int main() {
  // Every symbol's RGB565 contribution must be additive, never darken pixels.
  const uint16_t backdrop[] = {0x0000u, 0x39e7u, 0x07e0u, 0xf800u, 0xffffu};
  const uint16_t symbols[] = {0x001fu, 0x07e0u, 0xf800u, 0xffffu};
  for (uint16_t bg : backdrop) {
    for (uint16_t color : symbols) {
      assert(addCoverage565(bg, color, 0u) == bg);
      uint16_t last = bg;
      for (unsigned intensity = 0; intensity <= 4096; intensity += 16) {
        const uint16_t value = addCoverage565(bg, color, static_cast<uint16_t>(intensity));
        assert((value & 31u) >= (bg & 31u));
        assert(((value >> 5) & 63u) >= ((bg >> 5) & 63u));
        assert(((value >> 11) & 31u) >= ((bg >> 11) & 31u));
        assert((value & 31u) >= (last & 31u));
        assert(((value >> 5) & 63u) >= ((last >> 5) & 63u));
        assert(((value >> 11) & 31u) >= ((last >> 11) & 31u));
        last = value;
      }
    }
  }
  assert(addCoverage565(0xffffu, 0xffffu, 4096u) == 0xffffu);
  puts("V17.37 additive icon Q12 blending: OK");
}
