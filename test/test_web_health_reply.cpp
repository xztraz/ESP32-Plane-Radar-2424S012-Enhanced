#include "services/web_health_reply.h"

#include <cassert>
#include <cstring>
#include <string>

using services::web_health::Reply;

static void feed(Reply& reply, const std::string& bytes) {
  for (const char c : bytes) reply.append(static_cast<uint8_t>(c));
}

int main() {
  const std::string valid =
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
      "Content-Length: 26\r\nCache-Control: no-store\r\n\r\n"
      "Plane Radar web server OK\n";
  Reply reply;
  assert(reply.result() == Reply::Result::kPending);
  feed(reply, valid.substr(0, 15));
  assert(reply.result() == Reply::Result::kPending);
  feed(reply, valid.substr(15, 65));
  assert(reply.result() == Reply::Result::kPending);
  feed(reply, valid.substr(80));
  assert(reply.result() == Reply::Result::kOk);
  assert(reply.length() == valid.length());

  reply.reset();
  feed(reply, "HTTP/1.1 503 Unavailable\r\n\r\nPlane Radar web server OK\n");
  assert(reply.result() == Reply::Result::kBadStatus);

  reply.reset();
  feed(reply, "HTTP/1.1 200 OK\r\n\r\nWrong server\n");
  assert(reply.result() == Reply::Result::kPending); // Full expected body not yet received
  feed(reply, std::string(40, 'x'));
  assert(reply.result() == Reply::Result::kBadBody);

  reply.reset();
  feed(reply, "HTTP/1.1 302 Found\r\nLocation: /\r\n\r\n");
  assert(reply.result() == Reply::Result::kBadStatus);

  reply.reset();
  feed(reply, std::string(600, 'x'));
  assert(reply.result() == Reply::Result::kTooLarge);

  reply.reset();
  feed(reply, "HTTP/1.0 200 OK\r\n\r\nPlane Radar web server OK\n");
  assert(reply.result() == Reply::Result::kOk);
}
