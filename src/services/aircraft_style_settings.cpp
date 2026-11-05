#include "services/aircraft_style_settings.h"
#include "services/adsb_client.h"
#include <Preferences.h>
#include <cstring>

namespace services::aircraft_styles {
namespace {
constexpr char kNamespace[] = "acstyles";
constexpr char kKey[] = "colors";
using services::aircraft_palette::Selection;
using services::aircraft_palette::at;
struct State {
  // Defaults apply only when no valid color selection is stored in Preferences.
  // RGB332 9D = #92FF55, ED = #FF6D55, FD = #FFFF55.
  Selection normal = at(70, 68);       // #92FF55 · RGB332 9D
  Selection military = at(6, 68);      // #FF6D55 · RGB332 ED
  Selection helicopter = at(43, 68);   // #FFFF55 · RGB332 FD
  bool military_priority = false;
  bool helicopter_priority = false;
};
State s_state;
bool s_inited = false;

bool parse(const char* value, State* out) {
  if (!value || !out || strlen(value) > 48) return false;
  const char* cursor = value;
  Selection* selections[3] = {&out->normal, &out->military, &out->helicopter};
  for (auto* item : selections) {
    const char* comma = strchr(cursor, ',');
    if (!comma || !services::aircraft_palette::decode(
          cursor, static_cast<size_t>(comma - cursor), item)) return false;
    cursor = comma + 1;
  }
  if ((cursor[0] != '0' && cursor[0] != '1')) return false;
  out->military_priority = cursor[0] == '1';
  if (cursor[1] == '\0') { out->helicopter_priority = false; return true; }
  if (cursor[1] != ',' || (cursor[2] != '0' && cursor[2] != '1') ||
      cursor[3] != '\0') return false;
  out->helicopter_priority = cursor[2] == '1';
  return true;
}
}  // namespace
void init() {
  if (s_inited) return;
  Preferences prefs;
  if (prefs.begin(kNamespace, true)) {
    const String saved = prefs.getString(kKey, "");
    prefs.end();
    State candidate;
    if (parse(saved.c_str(), &candidate)) s_state = candidate;
  }
  s_inited = true;
}
String encoded() {
  init();
  char result[64];
  snprintf(result, sizeof(result), "%06lX%02X%02X,%06lX%02X%02X,%06lX%02X%02X,%u,%u",
      static_cast<unsigned long>(s_state.normal.rgb), s_state.normal.hue, s_state.normal.shade,
      static_cast<unsigned long>(s_state.military.rgb), s_state.military.hue, s_state.military.shade,
      static_cast<unsigned long>(s_state.helicopter.rgb), s_state.helicopter.hue, s_state.helicopter.shade,
      s_state.military_priority ? 1u : 0u, s_state.helicopter_priority ? 1u : 0u);
  return String(result);
}
bool save(const char* value) {
  State candidate;
  if (!parse(value, &candidate)) return false;
  if (encoded() == value) return true;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  const bool ok = prefs.putString(kKey, value) == strlen(value);
  prefs.end();
  if (ok) { s_state = candidate; s_inited = true; }
  return ok;
}
Selection defaultSelection(const services::adsb::Aircraft& plane) {
  init();
  if (plane.isMilitary) return s_state.military;
  return plane.isHelicopter ? s_state.helicopter : s_state.normal;
}
uint32_t defaultRgb(const services::adsb::Aircraft& plane) { return defaultSelection(plane).rgb; }
bool militaryPriority() { init(); return s_state.military_priority; }
bool helicopterPriority() { init(); return s_state.helicopter_priority; }
}  // namespace services::aircraft_styles
