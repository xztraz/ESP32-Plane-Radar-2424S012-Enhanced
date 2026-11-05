#include "ui/radar_range.h"

#include "ui/radar_theme.h"

#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui::radar {

namespace {

constexpr char kPrefsNamespace[] = "planeradar";
constexpr char kPrefsRangeKey[] = "rangeIdx";
constexpr char kPrefsMilesKey[] = "useMiles";
constexpr char kPrefsRunwaysKey[] = "showRwys";
constexpr char kPrefsBgThemeKey[] = "bgTheme";
constexpr char kPrefsBgPatternKey[] = "bgPattern";
constexpr uint8_t kDefaultRangeIndex = 1;  // 10 km ring
constexpr float kKmPerMile = 1.609344f;

Preferences s_prefs;
uint8_t s_range_index = kDefaultRangeIndex;
bool s_use_miles = false;
bool s_show_runways = true;
BackgroundTheme s_background_theme = BackgroundTheme::Green;
BackgroundPattern s_background_pattern = BackgroundPattern::Original;

void saveRangeIndex() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsRangeKey, s_range_index);
  s_prefs.end();
}

void saveUseMiles() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsMilesKey, s_use_miles);
  s_prefs.end();
}

void saveShowRunways() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsRunwaysKey, s_show_runways);
  s_prefs.end();
}

void saveBackgroundThemePref() {
  if (!s_prefs.begin(kPrefsNamespace, false)) return;
  s_prefs.putUChar(kPrefsBgThemeKey, static_cast<uint8_t>(s_background_theme));
  s_prefs.end();
}

void saveBackgroundPatternPref() {
  if (!s_prefs.begin(kPrefsNamespace, false)) return;
  s_prefs.putUChar(kPrefsBgPatternKey, static_cast<uint8_t>(s_background_pattern));
  s_prefs.end();
}

bool portalCheckboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  // WiFiManager checkbox submits its value= attribute ("T", or "F" if we prefilled F).
  if ((value[0] == 'T' || value[0] == 't' || value[0] == 'F' || value[0] == 'f') &&
      value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

}  // namespace

void rangeInit() {
  if (!s_prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const uint8_t saved = s_prefs.getUChar(kPrefsRangeKey, kDefaultRangeIndex);
  s_range_index =
      (saved < kRangePresetCount) ? saved : kDefaultRangeIndex;
  s_use_miles = s_prefs.getBool(kPrefsMilesKey, false);
  s_show_runways = s_prefs.getBool(kPrefsRunwaysKey, true);
  const uint8_t saved_theme = s_prefs.getUChar(kPrefsBgThemeKey, 0);
  s_background_theme = saved_theme <= 5 ? static_cast<BackgroundTheme>(saved_theme)
                                        : BackgroundTheme::Green;
  const uint8_t saved_pattern = s_prefs.getUChar(kPrefsBgPatternKey, 0);
  s_background_pattern = saved_pattern <= 5 ? static_cast<BackgroundPattern>(saved_pattern)
                                            : BackgroundPattern::Original;
  s_prefs.end();
}

void rangeSetIndex(uint8_t index) {
  if (index >= kRangePresetCount || index == s_range_index) return;
  s_range_index = index;
  saveRangeIndex();
}

void rangeNext() {
  s_range_index = static_cast<uint8_t>((s_range_index + 1) % kRangePresetCount);
  saveRangeIndex();
}

const RangePreset& rangeCurrent() { return kRangePresets[s_range_index]; }

uint8_t rangeIndex() { return s_range_index; }
uint16_t redrawIntervalMs() { return rangeCurrent().redraw_interval_ms; }

// Search twice the *displayed third-ring* distance, rather than
// a little beyond the outer ring. E.g. 100 km setting -> 200 km query.
float fetchRadiusKm() {
  return rangeCurrent().ring3_km * 2.0f;
}

bool useMiles() { return s_use_miles; }

bool showRunways() { return s_show_runways; }

BackgroundTheme backgroundTheme() { return s_background_theme; }
BackgroundPattern backgroundPattern() { return s_background_pattern; }

void saveBackgroundTheme(uint8_t theme_index) {
  if (theme_index > 5) return;
  const BackgroundTheme selected = static_cast<BackgroundTheme>(theme_index);
  if (s_background_theme == selected) return;
  s_background_theme = selected;
  saveBackgroundThemePref();
  Serial.printf("Radar color theme: %u\r\n", static_cast<unsigned>(theme_index));
}

void saveBackgroundPattern(uint8_t pattern_index) {
  if (pattern_index > 5) return;
  const BackgroundPattern selected = static_cast<BackgroundPattern>(pattern_index);
  if (s_background_pattern == selected) return;
  s_background_pattern = selected;
  saveBackgroundPatternPref();
  Serial.printf("Radar background pattern: %u\r\n",
                static_cast<unsigned>(pattern_index));
}

void saveMilesFromPortal(const char* checkbox_value) {
  const bool selected = portalCheckboxChecked(checkbox_value);
  if (s_use_miles == selected) return;
  s_use_miles = selected;
  saveUseMiles();
  Serial.printf("Distance units: %s\r\n", s_use_miles ? "miles" : "km");
}

void saveRunwaysFromPortal(const char* checkbox_value) {
  const bool selected = portalCheckboxChecked(checkbox_value);
  if (s_show_runways == selected) return;
  s_show_runways = selected;
  saveShowRunways();
  Serial.printf("Runway overlay: %s\r\n", s_show_runways ? "on" : "off");
}

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  if (use_miles) {
    const int mi = static_cast<int>(lroundf(ring3_km / kKmPerMile));
    snprintf(buf, len, "%dmi", mi);
  } else {
    const int km = static_cast<int>(lroundf(ring3_km));
    snprintf(buf, len, "%dkm", km);
  }
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

void unitsReset() {
  s_use_miles = false;
  s_show_runways = true;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.remove(kPrefsMilesKey);
    s_prefs.remove(kPrefsRunwaysKey);
    s_prefs.end();
  }
}

}  // namespace ui::radar
