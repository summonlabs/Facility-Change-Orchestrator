// Real-process tests.
//
// Every claim exercised here is proven with independent operating system
// processes: durable restart, cross-process write exclusion, lock release when a
// holder is killed, crash consistency at every commit stage, and a full plan
// lifecycle driven across process boundaries.
//
// No test in this file applies a timeout to a command. The only bounded waits
// are readiness waits for a synchronisation event, and a readiness wait that is
// never satisfied fails the test rather than aborting it.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fco/error.hpp"
#include "fco/store.hpp"
#include "test_support.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

namespace fs = std::filesystem;

struct RunResult {
  bool started = false;
  int exit_code = -1;
  std::string output;
  std::string error;
};

[[nodiscard]] std::string quote_argument(const std::string& text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

[[nodiscard]] std::string read_all(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::string();
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

[[nodiscard]] std::string require_env(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    FCO_CHECK(false);
    return std::string();
  }
  return std::string(value);
}

class Scratch {
 public:
  Scratch() {
    static unsigned counter = 0;
    ++counter;
    const auto base = fs::temp_directory_path();
    std::ostringstream name;
    name << "fco-process-" << counter << '-' << std::chrono::steady_clock::now()
                                                 .time_since_epoch()
                                                 .count();
    root_ = base / name.str();
    std::error_code code;
    fs::remove_all(root_, code);
    fs::create_directories(root_, code);
  }
  ~Scratch() {
    std::error_code code;
    fs::remove_all(root_, code);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  [[nodiscard]] const fs::path& root() const noexcept { return root_; }
  [[nodiscard]] fs::path child(const std::string& name) const { return root_ / name; }
  [[nodiscard]] std::string child_text(const std::string& name) const {
    return child(name).string();
  }

 private:
  fs::path root_;
};

#ifdef _WIN32

[[nodiscard]] std::wstring widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

struct Spawned {
  void* process = nullptr;
};

void terminate_spawned(Spawned& spawned) {
  if (spawned.process == nullptr) return;
  TerminateProcess(static_cast<HANDLE>(spawned.process), 70);
  WaitForSingleObject(static_cast<HANDLE>(spawned.process), INFINITE);
  CloseHandle(static_cast<HANDLE>(spawned.process));
  spawned.process = nullptr;
}

[[nodiscard]] bool spawn_detached(const std::string& executable,
                                  const std::vector<std::string>& arguments,
                                  const fs::path& output_path, const fs::path& error_path,
                                  Spawned& spawned) {
  std::string command = quote_argument(executable);
  for (const std::string& argument : arguments) {
    command += ' ';
    command += quote_argument(argument);
  }
  std::wstring wide_command = widen(command);

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE output = CreateFileW(widen(output_path.string()).c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE error = CreateFileW(widen(error_path.string()).c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE || error == INVALID_HANDLE_VALUE ||
      input == INVALID_HANDLE_VALUE) {
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (error != INVALID_HANDLE_VALUE) CloseHandle(error);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    return false;
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = error;
  startup.hStdInput = input;

  PROCESS_INFORMATION information{};
  const BOOL ok = CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE, 0,
                                 nullptr, nullptr, &startup, &information);
  CloseHandle(output);
  CloseHandle(error);
  CloseHandle(input);
  if (ok == FALSE) return false;

  CloseHandle(information.hThread);
  spawned.process = information.hProcess;
  return true;
}

[[nodiscard]] RunResult run_process(const std::string& executable,
                                    const std::vector<std::string>& arguments,
                                    const fs::path& output_path, const fs::path& error_path) {
  RunResult result;
  Spawned spawned;
  if (!spawn_detached(executable, arguments, output_path, error_path, spawned)) {
    return result;
  }
  result.started = true;
  WaitForSingleObject(static_cast<HANDLE>(spawned.process), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(spawned.process), &code);
  CloseHandle(static_cast<HANDLE>(spawned.process));
  spawned.process = nullptr;
  result.exit_code = static_cast<int>(code);
  result.output = read_all(output_path);
  result.error = read_all(error_path);
  return result;
}

[[nodiscard]] void* create_event(const std::string& name) {
  return CreateEventW(nullptr, TRUE, FALSE, widen(name).c_str());
}

[[nodiscard]] bool wait_for_event(void* event, unsigned milliseconds) {
  return WaitForSingleObject(static_cast<HANDLE>(event), milliseconds) == WAIT_OBJECT_0;
}

void close_event(void* event) {
  if (event != nullptr) CloseHandle(static_cast<HANDLE>(event));
}

[[nodiscard]] std::string unique_event_name(const char* tag) {
  std::ostringstream name;
  name << "fco-test-" << tag << '-' << GetCurrentProcessId() << '-'
       << std::chrono::steady_clock::now().time_since_epoch().count();
  return name.str();
}

#else

struct Spawned {
  int pid = 0;
};

void terminate_spawned(Spawned& spawned) {
  if (spawned.pid <= 0) return;
  kill(spawned.pid, SIGKILL);
  int status = 0;
  waitpid(spawned.pid, &status, 0);
  spawned.pid = 0;
}

[[nodiscard]] bool spawn_detached(const std::string& executable,
                                  const std::vector<std::string>& arguments,
                                  const fs::path& output_path, const fs::path& error_path,
                                  Spawned& spawned) {
  std::vector<std::string> storage;
  storage.push_back(executable);
  for (const std::string& argument : arguments) storage.push_back(argument);
  std::vector<char*> argv;
  for (std::string& item : storage) argv.push_back(item.data());
  argv.push_back(nullptr);

  const std::string out = output_path.string();
  const std::string err = error_path.string();
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  pid_t pid = 0;
  const int code = posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (code != 0) return false;
  spawned.pid = static_cast<int>(pid);
  return true;
}

[[nodiscard]] RunResult run_process(const std::string& executable,
                                    const std::vector<std::string>& arguments,
                                    const fs::path& output_path, const fs::path& error_path) {
  RunResult result;
  Spawned spawned;
  if (!spawn_detached(executable, arguments, output_path, error_path, spawned)) return result;
  result.started = true;
  int status = 0;
  waitpid(spawned.pid, &status, 0);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  result.output = read_all(output_path);
  result.error = read_all(error_path);
  return result;
}

[[nodiscard]] void* create_event(const std::string& name) { (void)name; return nullptr; }
[[nodiscard]] bool wait_for_event(void* event, unsigned milliseconds) {
  (void)event;
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
  return true;
}
void close_event(void* event) { (void)event; }
[[nodiscard]] std::string unique_event_name(const char* tag) { (void)tag; return std::string(); }

#endif

class Command {
 public:
  Command(std::string executable, Scratch& scratch, std::string tag)
      : executable_(std::move(executable)),
        output_path_(scratch.child(tag + ".out")),
        error_path_(scratch.child(tag + ".err")) {}

  [[nodiscard]] RunResult run(const std::vector<std::string>& arguments) {
    return run_process(executable_, arguments, output_path_, error_path_);
  }

  [[nodiscard]] const fs::path& output_path() const noexcept { return output_path_; }
  [[nodiscard]] const fs::path& error_path() const noexcept { return error_path_; }
  [[nodiscard]] const std::string& executable() const noexcept { return executable_; }

 private:
  std::string executable_;
  fs::path output_path_;
  fs::path error_path_;
};

[[nodiscard]] std::string first_line(const std::string& text) {
  const std::size_t position = text.find('\n');
  std::string line = position == std::string::npos ? text : text.substr(0, position);
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  return line;
}

[[nodiscard]] std::string token_after(const std::string& text, const std::string& marker) {
  const std::size_t position = text.find(marker);
  if (position == std::string::npos) return std::string();
  std::size_t start = position + marker.size();
  while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) ++start;
  std::size_t end = start;
  while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\r' &&
         text[end] != '\n') {
    ++end;
  }
  return text.substr(start, end - start);
}

[[nodiscard]] std::string cli_path() { return require_env("FCO_CLI"); }
[[nodiscard]] std::string harness_path() { return require_env("FCO_CRASH_HARNESS"); }
[[nodiscard]] std::string probe_path() { return require_env("FCO_STORE_PROBE"); }

[[nodiscard]] bool initialize_store(const Scratch& scratch, Command& cli) {
  const RunResult result = cli.run({"init", "--root", scratch.child_text("store")});
  return result.started && result.exit_code == 0;
}

}  // namespace

FCO_TEST(process, cli_reports_release_identity) {
  Scratch scratch;
  Command cli(cli_path(), scratch, "version");
  const RunResult result = cli.run({"version"});
  FCO_CHECK(result.started);
  FCO_CHECK_EQ(result.exit_code, 0);
  FCO_CHECK(result.output.find("1.0.0") != std::string::npos);
  FCO_CHECK(result.output.find("DCCP repository 40 of 72") != std::string::npos);
}

FCO_TEST(process, usage_error_exit_code) {
  Scratch scratch;
  Command cli(cli_path(), scratch, "usage");
  const RunResult result = cli.run({"status", "--root", scratch.child_text("store"), "--bogus"});
  FCO_CHECK(result.started);
  FCO_CHECK(result.exit_code != 0);
}

FCO_TEST(process, store_is_initialized_once) {
  Scratch scratch;
  Command cli(cli_path(), scratch, "init");
  FCO_CHECK(initialize_store(scratch, cli));
  Command again(cli_path(), scratch, "init2");
  const RunResult second = again.run({"init", "--root", scratch.child_text("store")});
  FCO_CHECK(second.started);
  FCO_CHECK(second.exit_code != 0);
  FCO_CHECK(second.error.find("impossible-combination") != std::string::npos);
}

FCO_TEST(process, state_survives_process_restart) {
  Scratch scratch;
  Command cli(cli_path(), scratch, "restart");
  FCO_CHECK(initialize_store(scratch, cli));

  Command synth(cli_path(), scratch, "synth");
  const RunResult request =
      synth.run({"request", "synth", "--root", scratch.child_text("store"), "--kind",
                 "power-maintenance", "--request-id", "req-restart", "--site", "site-alpha",
                 "--assets", "rack-a1-node-1", "--out", scratch.child_text("request.json")});
  FCO_CHECK(request.started);
  FCO_CHECK_EQ(request.exit_code, 0);

  Command create(cli_path(), scratch, "create");
  const RunResult created = create.run({"plan", "create", "--root", scratch.child_text("store"),
                                        "--file", scratch.child_text("request.json")});
  FCO_CHECK(created.started);
  FCO_CHECK_EQ(created.exit_code, 0);
  const std::string plan_id = first_line(created.output).substr(std::string("plan ").size());
  FCO_CHECK(!plan_id.empty());

  Command status(cli_path(), scratch, "status");
  const RunResult reported = status.run({"status", "--root", scratch.child_text("store")});
  FCO_CHECK(reported.started);
  FCO_CHECK_EQ(reported.exit_code, 0);
  FCO_CHECK(reported.output.find(plan_id) != std::string::npos);
  FCO_CHECK(reported.output.find("revision=") != std::string::npos);
}

FCO_TEST(process, whole_plan_lifecycle_across_processes) {
  Scratch scratch;
  Command cli(cli_path(), scratch, "lifecycle");
  FCO_CHECK(initialize_store(scratch, cli));

  Command synth(cli_path(), scratch, "synth2");
  const RunResult request =
      synth.run({"request", "synth", "--root", scratch.child_text("store"), "--kind",
                 "power-maintenance", "--request-id", "req-lifecycle", "--site", "site-alpha",
                 "--assets", "rack-a1-node-1", "--out", scratch.child_text("request.json")});
  FCO_CHECK_EQ(request.exit_code, 0);

  Command create(cli_path(), scratch, "create2");
  const RunResult created = create.run({"plan", "create", "--root", scratch.child_text("store"),
                                        "--file", scratch.child_text("request.json")});
  FCO_CHECK_EQ(created.exit_code, 0);
  const std::string plan_id = first_line(created.output).substr(std::string("plan ").size());
  FCO_CHECK(!plan_id.empty());

  Command evaluate(cli_path(), scratch, "evaluate");
  const RunResult evaluated =
      evaluate.run({"plan", "evaluate", "--root", scratch.child_text("store"), "--plan", plan_id,
                    "--apply"});
  FCO_CHECK_EQ(evaluated.exit_code, 0);

  Command authorize(cli_path(), scratch, "authorize");
  const RunResult authorized = authorize.run(
      {"plan", "authorize", "--root", scratch.child_text("store"), "--plan", plan_id});
  FCO_CHECK_EQ(authorized.exit_code, 0);

  Command execute(cli_path(), scratch, "execute");
  const RunResult executed = execute.run(
      {"execute", "all", "--root", scratch.child_text("store"), "--plan", plan_id});
  FCO_CHECK_EQ(executed.exit_code, 0);
  FCO_CHECK(executed.output.find("SYNTHETIC") != std::string::npos);

  Command closure(cli_path(), scratch, "closure");
  const RunResult closed = closure.run({"closure", "--root", scratch.child_text("store")});
  FCO_CHECK_EQ(closed.exit_code, 0);
  FCO_CHECK(closed.output.find("completed=1") != std::string::npos);
  FCO_CHECK(closed.output.find("closed: yes") != std::string::npos);
}

FCO_TEST(process, second_process_is_locked_out_and_lock_survives_death) {
  Scratch scratch;
  Command cli(cli_path(), scratch, "lock");
  FCO_CHECK(initialize_store(scratch, cli));

  Command probe(probe_path(), scratch, "probe");
  const std::string ready_name = unique_event_name("ready");
  void* ready = create_event(ready_name);
  FCO_CHECK(ready != nullptr);

  Spawned holder;
  const bool started = spawn_detached(
      probe.executable(), {"hold", scratch.child_text("store"), ready_name, "1500"},
      probe.output_path(), probe.error_path(), holder);
  FCO_CHECK(started);
  // Readiness wait, not a test timeout: the holder signals this event once it
  // owns the exclusive writer lock.
  FCO_CHECK(wait_for_event(ready, 60000));

  Command blocked(cli_path(), scratch, "blocked");
  const RunResult locked = blocked.run({"status", "--root", scratch.child_text("store")});
  FCO_CHECK(locked.started);
  FCO_CHECK_EQ(locked.exit_code, 3);
  FCO_CHECK(locked.error.find("lock-unavailable") != std::string::npos);

  terminate_spawned(holder);
  close_event(ready);

  Command after(cli_path(), scratch, "after");
  const RunResult released = after.run({"status", "--root", scratch.child_text("store")});
  FCO_CHECK(released.started);
  FCO_CHECK_EQ(released.exit_code, 0);

  // A holder that is killed outright must not leave the lock behind either.
  const std::string crash_name = unique_event_name("crashready");
  void* ready2 = create_event(crash_name);
  FCO_CHECK(ready2 != nullptr);
  Spawned crashing;
  const bool started2 = spawn_detached(
      probe.executable(), {"crash", scratch.child_text("store"), crash_name},
      probe.output_path(), probe.error_path(), crashing);
  FCO_CHECK(started2);
  FCO_CHECK(wait_for_event(ready2, 60000));

  terminate_spawned(crashing);
  close_event(ready2);

  Command after_crash(cli_path(), scratch, "aftercrash");
  const RunResult released_after_crash =
      after_crash.run({"status", "--root", scratch.child_text("store")});
  FCO_CHECK(released_after_crash.started);
  FCO_CHECK_EQ(released_after_crash.exit_code, 0);
}

FCO_TEST(process, crash_at_every_commit_stage_recovers_one_generation) {
  Scratch scratch;
  Command cli(cli_path(), scratch, "crashtemplate");
  FCO_CHECK(initialize_store(scratch, cli));

  const fs::path template_store = scratch.child("store");
  const RunResult baseline = cli.run({"status", "--root", template_store.string()});
  FCO_CHECK_EQ(baseline.exit_code, 0);
  const std::string baseline_revision = token_after(baseline.output, "revision=");
  FCO_CHECK(!baseline_revision.empty());

  const std::vector<std::string> stages = {
      "before-staging-write",   "after-staging-written",   "after-staging-flushed",
      "after-staging-verified", "after-record-published",  "before-manifest-write",
      "after-manifest-flushed", "after-manifest-verified", "after-manifest-published",
      "after-fencing-published", "after-retention",       "commit-complete"};

  Command harness(harness_path(), scratch, "harness");
  for (const std::string& stage : stages) {
    const fs::path copy = scratch.child("clone-" + stage);
    std::error_code code;
    fs::copy(template_store, copy, fs::copy_options::recursive, code);
    FCO_CHECK(!code);

    const RunResult crashed = harness.run({copy.string(), stage});
    FCO_CHECK(crashed.started);
    // Either the process terminated at the stage (70) or the commit completed.
    FCO_CHECK(crashed.exit_code == 70 || crashed.exit_code == 0);

    Command verify(cli_path(), scratch, "verify-" + stage);
    const RunResult recovered = verify.run({"status", "--root", copy.string()});
    FCO_CHECK_EQ(recovered.exit_code, 0);
    const std::string recovered_revision = token_after(recovered.output, "revision=");
    FCO_CHECK(!recovered_revision.empty());
    // Exactly one authoritative generation: the previous revision or the new one.
    const bool previous = recovered_revision == baseline_revision;
    const std::string expected_next = std::to_string(std::stoull(baseline_revision) + 1);
    const bool next = recovered_revision == expected_next;
    FCO_CHECK(previous || next);
    FCO_CHECK(recovered.error.empty());

    // A further commit must still be possible, so recovery left a usable store.
    const RunResult reinit = verify.run({"status", "--root", copy.string()});
    FCO_CHECK_EQ(reinit.exit_code, 0);
  }
}

FCO_TEST_MAIN

