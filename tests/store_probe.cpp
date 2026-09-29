// Multiprocess lock probe used by the process test suite.
//
// This is a test helper, not part of the library: it exists so that lock
// exclusion and lock release on process death can be proven with real,
// independent operating system processes rather than with threads.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "fco/store.hpp"
#include "fco/state.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void signal_named_event(const std::string& name) {
#ifdef _WIN32
  if (name.empty()) return;
  const std::wstring wide(name.begin(), name.end());
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, wide.c_str());
  if (event == nullptr) return;
  SetEvent(event);
  CloseHandle(event);
#else
  (void)name;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: fco_store_probe <hold|crash|try> <root> [event] [milliseconds]\n";
    return 2;
  }
  const std::string mode = argv[1];
  const std::string root = argv[2];

  auto path = fco::path_from_utf8(root);
  if (!path.ok()) {
    std::cerr << "path: " << fco::describe(path.error()) << '\n';
    return 2;
  }

  auto store = fco::DurableStore::open(path.value());
  if (!store.ok()) {
    std::cout << (store.error().code == fco::ErrorCode::LockUnavailable ? "DENIED" : "FAILED")
              << '\n';
    std::cout.flush();
    return 3;
  }

  std::cout << "LOCKED\n";
  std::cout.flush();

  if (mode == "try") {
    store.value().close();
    std::cout << "ACQUIRED\n";
    std::cout.flush();
    return 0;
  }

  const std::string event_name = argc > 3 ? argv[3] : std::string();
  signal_named_event(event_name);

  if (mode == "crash") {
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 70);
#else
    std::_Exit(70);
#endif
  }

  if (mode == "hold") {
    std::uint64_t milliseconds = 2000;
    if (argc > 4) {
      const long parsed = std::strtol(argv[4], nullptr, 10);
      if (parsed > 0) milliseconds = static_cast<std::uint64_t>(parsed);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    store.value().close();
    return 0;
  }

  std::cerr << "unknown mode\n";
  return 2;
}
