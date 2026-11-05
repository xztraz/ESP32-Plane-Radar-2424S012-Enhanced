#include "ui/trail_fade.h"
#include <cstring>

namespace ui { namespace trail_fade {
namespace {
// All four lookup curves reside in read-only program flash. Values are
// relative palette brightness in Q12 at normalized ages 0..31. Generated offline: no powf/sqrtf
// or extra work during the radar redraw.
constexpr uint16_t kCurvesQ12[4][kCurveEntries] = {
  // Smooth S
  {4096, 4083, 4047, 3988, 3909, 3811, 3695, 3564, 3418, 3261, 3092, 2915, 2730, 2539, 2344, 2147, 1949, 1752, 1557, 1366, 1181, 1004, 835, 678, 532, 401, 285, 187, 108, 49, 13, 0},
  // Linear
  {4096, 3964, 3832, 3700, 3567, 3435, 3303, 3171, 3039, 2907, 2775, 2643, 2510, 2378, 2246, 2114, 1982, 1850, 1718, 1586, 1453, 1321, 1189, 1057, 925, 793, 661, 529, 396, 264, 132, 0},
  // Phosphor
  {4096, 3223, 2903, 2664, 2466, 2294, 2140, 1999, 1869, 1748, 1634, 1526, 1424, 1326, 1232, 1141, 1054, 970, 889, 810, 733, 658, 586, 515, 446, 378, 312, 247, 183, 121, 60, 0},
  // Late Fade
  {4096, 4096, 4095, 4092, 4087, 4079, 4066, 4049, 4026, 3996, 3959, 3913, 3858, 3794, 3719, 3632, 3533, 3421, 3294, 3153, 2996, 2823, 2632, 2423, 2195, 1948, 1679, 1390, 1078, 743, 384, 0},
};
// Exactly ONE 64-byte active table is kept in RAM for the renderer.
uint16_t s_activeQ12[kCurveEntries] = {};
uint8_t s_selected = 0;
bool s_ready = false;
void initIfNeeded() {
  if (s_ready) return;
  std::memcpy(s_activeQ12, kCurvesQ12[0], sizeof(s_activeQ12));
  s_ready = true;
}
}  // namespace

void selectCurve(uint8_t curve) {
  if (curve >= kCurveCount) curve = 0;
  if (s_ready && s_selected == curve) return;
  std::memcpy(s_activeQ12, kCurvesQ12[curve], sizeof(s_activeQ12));
  s_selected = curve;
  s_ready = true;
}
uint8_t selectedCurve() {
  initIfNeeded();
  return s_selected;
}
const uint16_t* activeCurveQ12() {
  initIfNeeded();
  return s_activeQ12;
}

}} // namespace ui::trail_fade
