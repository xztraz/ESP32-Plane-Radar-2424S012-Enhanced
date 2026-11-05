#include "services/http_response_head.h"
#include "services/http_body_framing.h"
#include <cassert>
#include <string>

struct ByteSource {
  std::string content;
  size_t pos = 0;
  int read() { return pos < content.size() ? (unsigned char)content[pos++] : -1; }
};

int main() {
  using namespace services::http;
  ResponseHead head;
  // The JSON {"ac":[]} is split across arbitrary chunk boundaries.
  ByteSource chunked{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                     "4\r\n{\"ac\r\n5\r\n\":[]}\r\n0\r\n\r\n"};
  assert(readResponseHead(chunked, &head));
  assert(head.status == 200 && head.chunked);
  BodyFramer<ByteSource> body(chunked, BodyFraming::kChunked, -1);
  std::string json;
  for (int c = body.read(); c >= 0; c = body.read()) json.push_back((char)c);
  assert(json == "{\"ac\":[]}");
  assert(body.complete() && !body.truncated() && !body.framingError());

  ByteSource length{"HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\n{\"ac\":[]}"};
  assert(readResponseHead(length, &head));
  assert(!head.chunked && head.content_length == 9);
  BodyFramer<ByteSource> body2(length, BodyFraming::kIdentity, head.content_length);
  json.clear();
  for (int c = body2.read(); c >= 0; c = body2.read()) json.push_back((char)c);
  assert(json == "{\"ac\":[]}" && body2.complete());

  ByteSource short_body{"HTTP/1.1 200 OK\r\nContent-Length: 30\r\n\r\n{\"ac\":[]}"};
  assert(readResponseHead(short_body, &head));
  BodyFramer<ByteSource> body3(short_body, BodyFraming::kIdentity, head.content_length);
  for (int c = body3.read(); c >= 0; c = body3.read()) {}
  assert(body3.truncated() && !body3.complete());

  ByteSource bad_header{"HTTP/1.1 200 OK\r\nX-Long: " + std::string(500, 'a') + "\r\n\r\n"};
  assert(!readResponseHead(bad_header, &head));
  return 0;
}
