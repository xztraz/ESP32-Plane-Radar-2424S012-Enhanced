#include "services/http_response_head.h"
#include <cassert>
#include <string>
#include <iostream>

struct Reader {
  std::string bytes;
  size_t offset = 0;
  int read() { return offset < bytes.size() ? (unsigned char)bytes[offset++] : -1; }
};
int main() {
  services::http::ResponseHead h;
  Reader a{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n"};
  assert(services::http::readResponseHead(a,&h));
  assert(h.status == 200 && h.chunked && !h.close);
  Reader b{"HTTP/1.1 200 OK\r\nContent-Length: 25\r\nConnection: close\r\n\r\n"};
  assert(services::http::readResponseHead(b,&h));
  assert(h.status == 200 && h.content_length == 25 && h.close && !h.chunked);
  Reader c{"HTTP/1.1 302 Found\r\nLocation: https://example.invalid\r\n\r\n"};
  assert(services::http::readResponseHead(c,&h) && h.status == 302);
  Reader d{"HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n"};
  assert(!services::http::readResponseHead(d,&h));
  Reader e{"HTTP/1.1 200 OK\r\nX-Long: " + std::string(1200,'x') + "\r\n\r\n"};
  assert(!services::http::readResponseHead(e,&h));
  Reader f{"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n{\"ac\":[]}"};
  assert(services::http::readResponseHead(f,&h) && f.read() == '{');
  std::cout << "HTTP response-head test passed\n";
}
