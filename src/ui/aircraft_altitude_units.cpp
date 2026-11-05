#include "ui/aircraft_altitude_units.h"
#include <Preferences.h>
#include <cstring>
namespace ui::altitude_units {
namespace {
  bool s_ready = false;
  bool s_meters = false;
  constexpr char kNamespace[] = "pr_altunit";
}
void init() {
  if (s_ready) return;
  Preferences prefs;
  if (prefs.begin(kNamespace,true)) {
    s_meters = prefs.getBool("meters",false);
    prefs.end();
  }
  s_ready = true;
}
bool meters() { init(); return s_meters; }
bool save(const char* value) {
  if (!value || (strcmp(value,"m") != 0 && strcmp(value,"ft") != 0)) return false;
  const bool selected = strcmp(value,"m") == 0;
  init();
  if (selected == s_meters) return true;
  Preferences prefs;
  if (!prefs.begin(kNamespace,false)) return false;
  const size_t written = prefs.putBool("meters",selected);
  prefs.end();
  if (written) { s_ready = true; s_meters = selected; return true; }
  return false;
}
}
