#include "ui/trail_dots.h"
#include "ui/trail_compositing.h"
#include <cassert>
#include <cstdint>

int main() {
  using namespace ui::trail_dots;
  static_assert(kDefaultSpacing == 3, "new marker spacing");
  static_assert(kDefaultDiameterHalfPx == 9, "4.5px marker");
  assert(nextPhase(0, 3) == 1);
  assert(nextPhase(1, 3) == 2);
  assert(nextPhase(2, 3) == 0);
  assert(nextPhase(0, 1) == 0);
  assert(nextPhase(2, 0) == 0);
  // Each recorded marker stays attached to the same fix after adding more
  // samples or rotating the 32-entry ring buffer. It is NEVER tied to index.
  for (uint8_t spacing = 1; spacing <= 8; ++spacing) {
    uint8_t newest_phase = 0;
    for (unsigned insert = 0; insert < 250; ++insert) {
      if (insert) newest_phase = nextPhase(newest_phase, spacing);
      for (unsigned age = 0; age <= 31 && age <= insert; ++age) {
        const bool marker = hasDot(newest_phase, spacing,
                                   static_cast<uint8_t>(age));
        assert(marker == ((insert - age) % spacing == 0));
      }
    }
  }
  assert(!hasDot(0, 0, 0));
  assert(!hasDot(0, 9, 0));
  // 10m fixed-point offset: at 5 km ring, 1km is 16.05 pixels.
  assert(offsetQ4(100, 5, 107) == 257);
  assert(offsetQ4(-100, 5, 107) == -257);
  assert(offsetQ4(0, 5, 107) == 0);
  assert(offsetQ4(100, 100, 107) == 13);
  assert(offsetQ4(100, 0, 107) == 0);
  // AA: empty, only shoulders, and full core. Monotonic intermediate levels.
  assert(coverageQ12(0, 0) == 0);
  assert(coverageQ12(0, 16) == 2867);
  assert(coverageQ12(16, 16) == 4096);
  assert(coverageQ12(8, 16) > coverageQ12(0, 16));
  assert(coverageQ12(16, 16) > coverageQ12(8, 16));
  assert(coverageQ12(17, 17) == 0);
  assert(coverageQ12(2, 1) == 0);
  // Additive contribution (not overpainting / per-segment MAX).
  constexpr uint16_t green = 0x07e0;
  assert(ui::trail_compositing::addCoverage565(green, green, 2048) == green);
  return 0;
}
