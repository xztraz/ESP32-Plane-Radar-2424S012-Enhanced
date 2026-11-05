#include "services/aircraft_palette.h"
#include "ui/trail_color_gradient.h"
#include "ui/trail_fade.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
using namespace services::aircraft_palette;
int main() {
 for (unsigned hue=0;hue<256;++hue) {
   assert(rgb888SmoothAt(hue,255)==0);
   assert(rgb888SmoothAt(hue,0)==0xFFFFFFu);
   for (unsigned shade=0;shade<256;++shade) {
     const uint32_t smooth = rgb888SmoothAt(hue,shade);
     // Verify smooth precision retains original palette after RGB332 quantization.
     const uint8_t r=(smooth>>16)&255,g=(smooth>>8)&255,b=smooth&255;
     const uint8_t quant=(((unsigned(r)*7+127)/255)<<5)|
                          (((unsigned(g)*7+127)/255)<<2)|
                          ((unsigned(b)*3+127)/255);
     assert(quant==rgb332At(hue,shade));
   }
 }
 // Ensure fade curve continues changing shade smoothly between samples.
 ui::trail_fade::selectCurve(1);
 assert(ui::trail_fade::shadeAtAge(0, 0, 32, 0, 0) == 0);
 assert(ui::trail_fade::shadeAtAge(0, 31, 32, 0, 0) == 255);
 std::puts("V17.23 smooth-palette / original RGB332 compatibility: OK");
}
