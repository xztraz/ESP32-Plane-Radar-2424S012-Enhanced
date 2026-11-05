// Host-side unit tests: g++ -std=c++17 -O2 -Iinclude src/ui/turn_prediction.cpp test/test_turn_prediction.cpp -o /tmp/turn-test
#include "ui/turn_prediction.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdio>

using services::adsb::Aircraft;
using ui::turn_prediction::Pose;
using ui::turn_prediction::predict;

Aircraft make(float nose, float track, float speed = 160.0f) {
  Aircraft a{};
  a.lat=59.5f; a.lon=17.4f; a.nose_deg=nose; a.track_deg=track;
  a.gs_knots=speed; a.pos_age_ms=0;
  return a;
}
float headingDelta(float a,float b){ float d=std::fmod(b-a+540.0f,360.0f)-180.0f;return d; }

int main(){
  constexpr float cosLatKm = 111.0f * 0.5075f;
  { // Heading wrap: 359->1 is a +2 degree turn, not -358.
    ui::turn_prediction::reset();
    auto a=make(359,359);
    predict("plane",a,10000,10000,cosLatKm);
    a.nose_deg=1; a.track_deg=1;
    Pose p=predict("plane",a,15000,15000,cosLatKm);
    assert(p.track_rate_deg_s > .30f && p.track_rate_deg_s < .50f);
    Pose later=predict("plane",a,15000,17000,cosLatKm);
    assert(headingDelta(p.nose_deg,later.nose_deg)>0);
    assert(later.turning);
    printf("wrap: rate %.3f deg/s; nose %.2f -> %.2f\n",p.track_rate_deg_s,p.nose_deg,later.nose_deg);
  }
  { // Stop turn on confirmed straight-flight sample, no long-term spin.
    ui::turn_prediction::reset();
    auto a=make(90,90);
    predict("plane",a,10000,10000,cosLatKm);
    a.nose_deg=100; a.track_deg=100;
    Pose p=predict("plane",a,15000,15000,cosLatKm);
    assert(p.turning && p.track_rate_deg_s>1.9f);
    a.nose_deg=100; a.track_deg=100;
    Pose straight=predict("plane",a,20000,20000,cosLatKm);
    assert(!straight.turning && straight.track_rate_deg_s==0.0f);
    Pose late=predict("plane",a,20000,31000,cosLatKm);
    assert(!late.turning);
    assert(fabsf(headingDelta(late.track_deg,100)) < 0.01f);
    printf("straight: rate %.2f; late ground track %.2f\n",straight.track_rate_deg_s,late.track_deg);
  }
  { // High speed turn for a helicopter; not capped at standard-rate 3deg/s.
    ui::turn_prediction::reset();
    auto a=make(15,15,65);
    predict("helo",a,10000,10000,cosLatKm);
    a.track_deg=115; a.nose_deg=115;
    Pose p=predict("helo",a,15000,15000,cosLatKm);
    assert(p.track_rate_deg_s > 19.9f && p.track_rate_deg_s < 20.1f);
    Pose later=predict("helo",a,15000,17000,cosLatKm);
    assert(headingDelta(115,later.track_deg)>30.0f);
    printf("helicopter: rate %.2f, predicted track %.2f\n",p.track_rate_deg_s,later.track_deg);
  }
  { // Geometry bends, but stays continuous with a straight-line zero-rate case.
    auto a=make(0,0,200);
    float straightLat=0,straightLon=0,turnLat=0,turnLon=0;
    ui::turn_prediction::positionAt(a,0,5,cosLatKm,&straightLat,&straightLon);
    ui::turn_prediction::positionAt(a,3,5,cosLatKm,&turnLat,&turnLon);
    assert(fabsf(straightLon-a.lon)<1e-6f);
    assert(turnLon > a.lon);
    assert(turnLat > a.lat);
    printf("arc: east longitude %.6f, straight longitude %.6f\n",turnLon,straightLon);
  }
  { // Old samples stop extrapolating turn beyond 9s and position beyond 12s.
    ui::turn_prediction::reset();
    auto a=make(0,0);
    predict("old",a,10000,10000,cosLatKm);
    a.track_deg=10;a.nose_deg=10;
    predict("old",a,15000,15000,cosLatKm);
    Pose p1=predict("old",a,15000,25000,cosLatKm);
    Pose p2=predict("old",a,15000,70000,cosLatKm);
    assert(fabsf(headingDelta(p1.track_deg,p2.track_deg))<0.001f);
    assert(fabsf(p1.lat-p2.lat)<.01f);
    printf("stale: frozen turn heading %.3f\n",p2.track_deg);
  }
  puts("all motion tests passed");
}
