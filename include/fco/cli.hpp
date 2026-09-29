#pragma once

// Command line surface.
//
// The CLI is a thin projection of the library: every command opens the durable
// store, drives the orchestrator, and reports what actually happened. It never
// performs a domain action, and it never invents an observation.

#include <string>
#include <vector>

namespace fco::cli {

// Runs one invocation. Returns the process exit code:
//   0  success
//   2  usage error
//   3  the durable store is locked by another process
//   4  durable state could not be recovered or committed
//   5  an attempt outcome is unresolved and must be resolved before continuing
//   1  any other failure
[[nodiscard]] int run(const std::vector<std::string>& arguments, std::string& output,
                      std::string& error_output);

[[nodiscard]] std::string usage();

// Exit codes, named so the tests and the documentation cannot drift apart.
inline constexpr int kExitSuccess = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitLocked = 3;
inline constexpr int kExitStorage = 4;
inline constexpr int kExitUnresolved = 5;

}  // namespace fco::cli
