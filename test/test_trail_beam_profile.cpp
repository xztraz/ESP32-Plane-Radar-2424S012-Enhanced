#include "ui/trail_beam_profile.h"
#include <cassert>
#include <cstdint>
int main() {
  using namespace ui::trail_beam;
  static_assert(kOuterRadiusSqQ8 == 655, "total trace ~3.2px");
  static_assert(kCoreRadiusSqQ8 == 92, "bright core ~1.2px");
  assert(intensityQ12(0, 0) == 0);
  assert(intensityQ12(0, 16) == 2048);
  assert(intensityQ12(16, 16) == 4096);
  assert(intensityQ12(8, 16) == 3072);
  assert(intensityQ12(1, 1) > intensityQ12(0, 1));
  assert(intensityQ12(17, 17) == 0);
  assert(intensityQ12(2, 1) == 0);
  return 0;
}
