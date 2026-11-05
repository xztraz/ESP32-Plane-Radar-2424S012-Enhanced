// Host test: g++ -std=c++11 -O2 -pthread -Iinclude test/test_fetch_save_gate.cpp -o /tmp/test-gate && /tmp/test-gate
#include "services/fetch_save_gate.h"
#include <cassert>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>

int main() {
  services::FetchSaveGate g;
  assert(g.tryBeginFetch(100));
  assert(!g.tryBeginSave(101));     // reject commit while TLS owns gate
  assert(!g.reserveSaveWindow(120)); // preflight fails while TLS active
  g.endFetch();
  assert(!g.tryBeginFetch(1000));  // retry window prevents next TLS fetch
  assert(g.reserveSaveWindow(1100));
  assert(g.tryBeginSave(1101));
  assert(!g.tryBeginFetch(1200));  // no TLS overlap with NVS commit
  g.endSave(1300);
  assert(!g.tryBeginFetch(1500));  // quiet window after save
  assert(g.tryBeginFetch(2301));
  g.endFetch();

  services::FetchSaveGate wrap;
  const uint32_t near_wrap=0xFFFFFF00u;
  assert(wrap.tryBeginSave(near_wrap));
  wrap.endSave(near_wrap);
  assert(!wrap.tryBeginFetch(0xFFFFFF20u));
  assert(!wrap.tryBeginFetch(20u));
  assert(wrap.tryBeginFetch(800u));
  wrap.endFetch();

  services::FetchSaveGate shared;
  std::atomic<int> concurrent(0),violations(0);
  std::vector<std::thread> tasks;
  for(int t=0;t<6;t++) tasks.emplace_back([&,t]{
    for(uint32_t i=0;i<40000;i++) {
      if(t%2 == 0) {
        if (shared.tryBeginFetch(100000 + i*10)) {
          if(concurrent.fetch_add(1) != 0) ++violations;
          concurrent.fetch_sub(1);
          shared.endFetch();
        }
      } else {
        if(shared.tryBeginSave(100000 + i*10)) {
          if(concurrent.fetch_add(1) != 0) ++violations;
          concurrent.fetch_sub(1);
          shared.endSave(100000 + i*10);
        }
      }
    }
  });
  for(auto &t : tasks) t.join();
  assert(violations == 0);
  printf("PASS: save/fetch exclusion, retry/quiet windows, millis wrap, 6-thread stress\n");
}
