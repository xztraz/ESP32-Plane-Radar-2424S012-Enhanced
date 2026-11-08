#pragma once

// Minimal, allocation-free verifier for the on-device HTTP /health response.
// Keeping it independent of Arduino also makes its parsing host-testable.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace services {
namespace web_health {

class Reply {
 public:
  enum class Result : uint8_t { kPending, kOk, kBadStatus, kBadBody, kTooLarge };
  static constexpr size_t kCapacity = 512;

  Reply() { reset(); }
  void reset() {
    length_ = 0;
    too_large_ = false;
    bytes_[0] = '\0';
  }

  void append(uint8_t byte) {
    if (length_ + 1 >= kCapacity) {
      too_large_ = true;
      return;
    }
    bytes_[length_++] = static_cast<char>(byte);
    bytes_[length_] = '\0';
  }

  size_t length() const { return length_; }

  Result result() const {
    if (too_large_) return Result::kTooLarge;
    // WebServer generates a normal HTTP/1.1 response with a fixed-length body.
    // Do not accept a bare TCP handshake, status line, or cached/empty response.
    const char* headers_end = strstr(bytes_, "\r\n\r\n");
    if (!headers_end) return Result::kPending;
    const bool status_ok =
        strncmp(bytes_, "HTTP/1.1 200 ", 13) == 0 ||
        strncmp(bytes_, "HTTP/1.0 200 ", 13) == 0;
    if (!status_ok) return Result::kBadStatus;
    const char* body = headers_end + 4;
    static const char kExpectedBody[] = "Plane Radar web server OK\n";
    constexpr size_t kExpectedLength = sizeof(kExpectedBody) - 1;
    const size_t received_body = length_ - static_cast<size_t>(body - bytes_);
    if (received_body < kExpectedLength) return Result::kPending;
    return memcmp(body, kExpectedBody, kExpectedLength) == 0
               ? Result::kOk : Result::kBadBody;
  }

 private:
  char bytes_[kCapacity];
  size_t length_;
  bool too_large_;
};

}  // namespace web_health
}  // namespace services
