#include "ui/trail_fade.h"
#include <cassert>
#include <cstdio>
using ui::trail_fade::alphaQ12;
using ui::trail_fade::selectCurve;
using ui::trail_fade::selectedCurve;
using ui::trail_fade::activeCurveQ12;
int main(){
  static const uint16_t snapshots[4][4] = {
    {4096,3988,2147,0},
    {4096,3700,2114,0},
    {4096,2664,1141,0},
    {4096,4092,3632,0}
  };
  for(unsigned c=0;c<4;c++){
    selectCurve(c);
    assert(selectedCurve()==c);
    const uint16_t* table=activeCurveQ12();
    assert(table[0]==snapshots[c][0]);
    assert(table[3]==snapshots[c][1]);
    assert(table[15]==snapshots[c][2]);
    assert(table[31]==snapshots[c][3]);
    for(unsigned k=1;k<32;k++) assert(table[k]<=table[k-1]);
    for(unsigned cap=2;cap<=32;cap++){
      for(unsigned age=0;age<cap;age++) {
        assert(alphaQ12(age,cap,0)<=4096);
        if(age+1<cap) assert(alphaQ12(age,cap,256)==alphaQ12(age+1,cap,0));
        uint16_t previous=4096;
        for(unsigned phase=0;phase<=256;phase++){
          const uint16_t v=alphaQ12(age,cap,phase);
          assert(v<=previous);
          previous=v;
        }
      }
    }
  }
  selectCurve(0);
  assert(alphaQ12(0,32,0)==4096);
  assert(alphaQ12(31,32,0)==0);
  selectCurve(99); // invalid IDs are clamped to the safe Smooth S default
  assert(selectedCurve()==0);
  std::puts("V17.19 four curves, RAM selection and time interpolation: OK");
}
