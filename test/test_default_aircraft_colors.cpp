#include "services/aircraft_palette.h"
#include <cassert>

int main() {
  using namespace services::aircraft_palette;
  const auto plane = at(70,68);
  const auto military = at(6,68);
  const auto helicopter = at(43,68);
  assert(plane.rgb == 0x92FF55u);
  assert(military.rgb == 0xFF6D55u);
  assert(helicopter.rgb == 0xFFFF55u);
  assert(rgb332At(plane.hue,plane.shade) == 0x9Du);
  assert(rgb332At(military.hue,military.shade) == 0xEDu);
  assert(rgb332At(helicopter.hue,helicopter.shade) == 0xFDu);
  return 0;
}
