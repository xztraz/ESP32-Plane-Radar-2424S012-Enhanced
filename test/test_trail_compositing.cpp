#include "ui/trail_compositing.h"
#include <cassert>
#include <cstdint>
int main() {
  using namespace ui::trail_compositing;
  constexpr uint16_t red = 0xf800;
  constexpr uint16_t orange = 0xfbc0;
  constexpr uint16_t blue = 0x001f;
  assert(scale565(red, 4096) == red);
  assert(scale565(red, 2048) == 0x8000);
  assert(scale565(orange, 0) == 0);
  // Same-aircraft overlapping segments: MAX, no self-additive brightening.
  const uint16_t core = scale565(orange, 4096);
  const uint16_t shoulder = scale565(orange, 2048);
  assert(max565(core, shoulder) == core);
  assert(max565(shoulder, core) == core);
  assert(max565(red, blue) == static_cast<uint16_t>(red | blue));
  // Different aircraft overlap: saturating ADD.
  assert(add565(red, blue) == static_cast<uint16_t>(red | blue));
  assert(add565(red, red) == red);
  assert(add565(0x7bef, 0x7bef) == 0xf7de);
  assert(addCoverage565(0, red, 2048) == 0x8000);
  return 0;
}
