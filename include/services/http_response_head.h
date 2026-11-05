#pragma once

#include <cstddef>
#include <cstdint>

namespace services::http {

struct ResponseHead {
  int status = 0;
  int content_length = -1;
  bool chunked = false;
  bool close = false;
};

inline bool asciiEqualIgnoreCase(char a, char b) {
  if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
  if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
  return a == b;
}
inline bool startsWithIgnoreCase(const char* text, const char* prefix) {
  while (*prefix) {
    if (!*text || !asciiEqualIgnoreCase(*text++, *prefix++)) return false;
  }
  return true;
}
inline bool containsIgnoreCase(const char* text, const char* target) {
  for (; *text; ++text) if (startsWithIgnoreCase(text, target)) return true;
  return false;
}
inline const char* skipSpace(const char* p) {
  while (*p == ' ' || *p == '\t') ++p;
  return p;
}

// Handles the standard HTTP/1.1 response head for BearSSL and the LAN proxy.
// Source.read() must return a byte or -1 on timeout/EOF.
// Strict 384-byte per line and 8 KiB total cap: malformed/oversized heads fail
// closed instead of consuming unbounded RAM.
template <typename Source>
bool readResponseHead(Source& source, ResponseHead* out) {
  if (!out) return false;
  *out = {};
  bool first = true;
  size_t total = 0;
  char line[384];
  for (unsigned lines = 0; lines < 64; ++lines) {
    size_t n = 0;
    for (;;) {
      const int ch = source.read();
      if (ch < 0 || ++total > 8192) return false;
      if (ch == '\n') break;
      if (ch == '\r') continue;
      if (n + 1 >= sizeof(line)) return false;
      line[n++] = static_cast<char>(ch);
    }
    line[n] = '\0';
    if (first) {
      first = false;
      if (!(startsWithIgnoreCase(line, "HTTP/1.1 ") ||
            startsWithIgnoreCase(line, "HTTP/1.0 "))) return false;
      const char* digits = line + 9;
      if (digits[0] < '1' || digits[0] > '5' ||
          digits[1] < '0' || digits[1] > '9' ||
          digits[2] < '0' || digits[2] > '9' ||
          (digits[3] != '\0' && digits[3] != ' ')) return false;
      out->status = (digits[0] - '0') * 100 +
                    (digits[1] - '0') * 10 + digits[2] - '0';
      continue;
    }
    if (n == 0) return out->status != 0;  // end of headers
    if (startsWithIgnoreCase(line, "Content-Length:")) {
      const char* p = skipSpace(line + 15);
      if (*p < '0' || *p > '9') return false;
      int value = 0;
      while (*p >= '0' && *p <= '9') {
        if (value > 20000000) return false;
        value = value * 10 + (*p++ - '0');
      }
      if (*skipSpace(p) != '\0') return false;
      out->content_length = value;
    } else if (startsWithIgnoreCase(line, "Transfer-Encoding:")) {
      out->chunked = containsIgnoreCase(line + 18, "chunked");
    } else if (startsWithIgnoreCase(line, "Connection:")) {
      out->close = containsIgnoreCase(line + 11, "close");
    }
  }
  return false;
}

}  // namespace services::http
