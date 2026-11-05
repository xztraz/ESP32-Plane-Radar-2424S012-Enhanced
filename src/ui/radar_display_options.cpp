#include "ui/radar_display_options.h"
#include <Preferences.h>
#include <cstdio>
#include <cstring>
namespace ui::display_options {
namespace {
constexpr char kNamespace[]="radaropts";
constexpr char kKey[]="display";
struct State { uint8_t fields=15; bool dots=false; uint16_t size=100; uint8_t brightness=100; };
State s_state;
bool s_ready=false;
bool parse(const char* value, State* result) {
  if (!value || !result || strlen(value)>24) return false;
  unsigned fields=0, dots=0, size=0, brightness=100;
  char extra='\0';
  const int read=sscanf(value,"%u,%u,%u,%u%c",
                        &fields,&dots,&size,&brightness,&extra);
  if (read != 4) {
    brightness = 100;
    if (sscanf(value,"%u,%u,%u%c",&fields,&dots,&size,&extra)!=3)
      return false; // Legacy settings have three fields.
  }
  if (fields>15 || dots>1 || size<10 || size>200 || size%10 ||
      brightness<20 || brightness>100 || brightness%10) return false;
  result->fields=static_cast<uint8_t>(fields);
  result->dots=dots!=0;
  result->size=static_cast<uint16_t>(size);
  result->brightness=static_cast<uint8_t>(brightness);
  return true;
}
}
void init() {
  if(s_ready)return;
  Preferences p;
  if(p.begin(kNamespace,true)) {
    const String stored=p.getString(kKey,"");p.end();
    State loaded;
    if(parse(stored.c_str(),&loaded))s_state=loaded;
  }
  s_ready=true;
}
String encoded() {
  init();
  char buf[32];
  snprintf(buf,sizeof(buf),"%u,%u,%u,%u",static_cast<unsigned>(s_state.fields),
           s_state.dots?1u:0u,static_cast<unsigned>(s_state.size),
           static_cast<unsigned>(s_state.brightness));
  return String(buf);
}
bool save(const char* value) {
  State next;
  if(!parse(value,&next))return false;
  if(encoded() == value)return true;
  Preferences p;
  if(!p.begin(kNamespace,false))return false;
  const bool ok=p.putString(kKey,value)==strlen(value);
  p.end();
  if(ok){s_state=next;s_ready=true;}
  return ok;
}
bool show(Field field) { init();return (s_state.fields & field)!=0; }
bool trailDots(){init();return s_state.dots;}
uint16_t iconScalePct(){init();return s_state.size;}
uint8_t labelBrightnessPct(){init();return s_state.brightness;}
float iconScale(){init();return static_cast<float>(s_state.size)*0.01f;}
}
