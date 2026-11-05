#include "services/aircraft_watchlist.h"
#include "services/adsb_client.h"
#include "services/aircraft_palette.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>

namespace services::watchlist {
namespace {
constexpr char kNamespace[] = "watchair";
constexpr char kKey[] = "entries";
constexpr size_t kMaxStored = 3700;
struct Entry {
  char id[14] = {};
  char name[17] = {};
  uint32_t rgb = 0;
  uint8_t hue = 0;
  uint8_t shade = 102;
  uint8_t kind = 'R';
  bool wildcard = false;
  bool priority = false;
};
Entry s_entries[kMaxWatched] = {};
size_t s_count = 0;
bool s_initialized = false;
SemaphoreHandle_t s_lock = nullptr;

char up(char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; }
int nibble(char c) {
  c = up(c);
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
bool equals(const char* a, const char* b) {
  while (*a && *b) if (up(*a++) != up(*b++)) return false;
  return *a == 0 && *b == 0;
}
bool prefix(const char* text, const char* part) {
  while (*part) {
    if (!*text || up(*text++) != up(*part++)) return false;
  }
  return true;
}
bool parse(const char* source, Entry* entries, size_t* count_out) {
  if (!source) return false;
  const size_t length = strnlen(source, kMaxStored + 1);
  if (length > kMaxStored) return false;
  const char* p = source;
  size_t count = 0;
  while (*p) {
    if (count == kMaxWatched) return false;
    const char* end = strchr(p, ';');
    if (!end) end = p + strlen(p);
    if (end == p || end - p < 12 || p[1] != ':' ||
        (p[0] != 'H' && p[0] != 'R' && p[0] != 'C')) return false;
    const char* a = static_cast<const char*>(memchr(p + 2, '|', end - p - 2));
    if (!a || a - p < 4 || end - a < 8) return false;
    const char* b = static_cast<const char*>(memchr(a + 1, '|', end - a - 1));
    if (!b || (b - a != 7 && b - a != 11)) return false;
    const char* c = static_cast<const char*>(memchr(b + 1, '|', end - b - 1));
    if (c && (end - c != 2 || (c[1] != '0' && c[1] != '1'))) return false;
    Entry e;
    e.kind = static_cast<uint8_t>(p[0]);
    const size_t input_len = static_cast<size_t>(a - (p + 2));
    e.wildcard = input_len > 0 && a[-1] == '*';
    const size_t idlen = input_len - (e.wildcard ? 1 : 0);
    const size_t max_id = e.kind == 'H' ? 6 : (e.kind == 'R' ? 12 : 8);
    if (idlen < (e.wildcard ? 1u : 2u) || idlen > max_id ||
        (!e.wildcard && e.kind == 'H' && idlen != 6) ||
        (e.wildcard && e.kind == 'H' && idlen >= 6)) return false;
    for (size_t i = 0; i < idlen; ++i) {
      char v = up(p[2 + i]);
      if (e.kind == 'H' ? nibble(v) < 0 :
          !((v >= 'A' && v <= 'Z') || (v >= '0' && v <= '9') ||
            (e.kind == 'R' && v == '-'))) return false;
      e.id[i] = v;
    }
    const size_t namelen = static_cast<size_t>((c ? c : end) - b - 1);
    if (namelen > 16) return false;
    for (size_t i = 0; i < namelen; ++i) {
      const char v = b[i + 1];
      if (!((v >= 'A' && v <= 'Z') || (v >= 'a' && v <= 'z') ||
            (v >= '0' && v <= '9') || v == ' ' || v == '-' || v == '_'))
        return false;
      e.name[i] = v;
    }
    services::aircraft_palette::Selection sel;
    if (!services::aircraft_palette::decode(a + 1,
          static_cast<size_t>(b - a - 1), &sel)) return false;
    e.rgb = sel.rgb;
    e.hue = sel.hue;
    e.shade = sel.shade;
    e.priority = c && c[1] == '1';
    for (size_t j = 0; j < count; ++j) {
      if (entries[j].kind == e.kind && entries[j].wildcard == e.wildcard &&
          equals(entries[j].id, e.id)) return false;
    }
    // Legacy V15 H:hex rules are retired in V16; other entries migrate.
    if (e.kind != 'H') entries[count++] = e;
    p = *end ? end + 1 : end;
    if (*end && !*p) return false;
  }
  *count_out = count;
  return true;
}
const char* identity(const services::adsb::Aircraft& plane, uint8_t kind) {
  return kind == 'C' ? plane.callsign : (kind == 'R' ? plane.registration : nullptr);
}
}  // namespace
void init() {
  if (s_initialized) return;
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return;
  Preferences prefs;
  if (prefs.begin(kNamespace, true)) {
    String data = prefs.getString(kKey, "");
    prefs.end();
    static Entry staging[kMaxWatched];
    size_t n = 0;
    if (parse(data.c_str(), staging, &n)) {
      memcpy(s_entries, staging, n * sizeof(Entry));
      s_count = n;
    }
  }
  s_initialized = true;
}
bool save(const char* value) {
  init();
  if (!s_lock || !value) return false;
  static Entry staging[kMaxWatched];
  size_t n = 0;
  if (!parse(value, staging, &n)) return false;
  if (encoded() == value) return true;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  const bool ok = *value ? prefs.putString(kKey, value) == strlen(value)
                         : (!prefs.isKey(kKey) || prefs.remove(kKey));
  prefs.end();
  if (!ok) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  memcpy(s_entries, staging, n * sizeof(Entry));
  s_count = n;
  xSemaphoreGive(s_lock);
  return true;
}
String encoded() {
  init();
  String result;
  if (!s_lock) return result;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (size_t i = 0; i < s_count; ++i) {
    const Entry& e = s_entries[i];
    if (i) result += ';';
    result += static_cast<char>(e.kind);
    result += ':';
    result += e.id;
    if (e.wildcard) result += '*';
    result += '|';
    char color[11];
    snprintf(color, sizeof(color), "%06lX%02X%02X",
             static_cast<unsigned long>(e.rgb & 0xFFFFFFu), e.hue, e.shade);
    result += color;
    result += '|';
    result += e.name;
    result += '|';
    result += e.priority ? '1' : '0';
  }
  xSemaphoreGive(s_lock);
  return result;
}
bool find(const services::adsb::Aircraft& plane, uint32_t* rgb,
          char* name, size_t name_len, bool* is_priority,
          uint8_t* hue, uint8_t* shade) {
  init();
  if (name && name_len) name[0] = 0;
  if (is_priority) *is_priority = false;
  if (hue) *hue = 0;
  if (shade) *shade = 102;
  if (!s_lock) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  int best = -1;
  int score = -1;
  for (size_t i = 0; i < s_count; ++i) {
    const Entry& e = s_entries[i];
    const char* id = identity(plane, e.kind);
    if (!id || !*id || !(e.wildcard ? prefix(id, e.id) : equals(id, e.id))) continue;
    // Exact wins over wildcard, then longest prefix; ties retain first entry.
    const int rating = (e.wildcard ? 0 : 1000) + strlen(e.id);
    if (rating > score) { score = rating; best = static_cast<int>(i); }
  }
  if (best >= 0) {
    const Entry& e = s_entries[best];
    if (rgb) *rgb = e.rgb;
    if (hue) *hue = e.hue;
    if (shade) *shade = e.shade;
    if (name && name_len) {
      strncpy(name, e.name, name_len - 1);
      name[name_len - 1] = '\0';
    }
    if (is_priority) *is_priority = e.priority;
  }
  xSemaphoreGive(s_lock);
  return best >= 0;
}
bool priority(const services::adsb::Aircraft& aircraft) {
  bool p = false;
  find(aircraft, nullptr, nullptr, 0, &p);
  return p;
}
}  // namespace services::watchlist
