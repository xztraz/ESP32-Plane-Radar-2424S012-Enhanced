#include "ui/trail_fade.h"
#include "services/aircraft_palette.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
using ui::trail_fade::alphaQ12;
using ui::trail_fade::shadeAtAge;
using ui::trail_fade::phaseQ8;
int main() {
  assert(phaseQ8(1000,1000,15000)==0);
  assert(phaseQ8(8500,1000,15000)==128);
  assert(phaseQ8(16000,1000,15000)==256);
  assert(phaseQ8(16001,1000,15000)==256);
  assert(phaseQ8(1000,0xfffffff0u,15000)==17);
  assert(alphaQ12(0,32,0)==4096);
  assert(alphaQ12(31,32,0)==0);
  assert(alphaQ12(3,32,0)>3950 && alphaQ12(3,32,0)<4050); // refined S ~97%
  assert(alphaQ12(0,32,256)==alphaQ12(1,32,0));
  for (size_t cap=2;cap<=32;++cap) {
    for (size_t age=0;age<cap;++age) {
      uint16_t prior=4096;
      for (uint16_t phase=0;phase<=256;++phase) {
        const uint16_t v=alphaQ12(age,cap,phase);
        assert(v <= prior);
        prior=v;
        for (int shade=0;shade<256;shade+=17) {
          for (int dark=0;dark<=100;dark+=25) {
            const uint8_t actual=shadeAtAge(shade,age,cap,phase,dark);
            if (phase==0 && age==0) {
              const int expected = shade + ((255-shade)*dark+50)/100;
              assert(actual == expected);
            }
            if (phase==256 && age+1<cap) {
              assert(actual==shadeAtAge(shade,age+1,cap,0,dark));
            }
          }
        }
      }
      if (age+1 < cap) {
        assert(alphaQ12(age,cap,256) == alphaQ12(age+1,cap,0));
      }
    }
    assert(alphaQ12(cap-1,cap,0)==0);
  }
  for (int shade=0;shade<=255;shade+=17) {
    assert(shadeAtAge(shade,0,24,0,0)==shade);
    assert(shadeAtAge(shade,0,24,0,100)==255);
  }
  puts("V17.20 palette-based curve shade and time interpolation: OK");
  return 0;
}
