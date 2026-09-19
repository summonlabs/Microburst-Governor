// Microburst Governor - real operating system process control.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/platform/process.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "mbg/core/hash.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <process.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace mbg::platform {
namespace {

std::atomic<std::uint64_t> g_incarnation_counter{0};

}  // namespace

std::uint64_t process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

IncarnationId mint_incarnation() noexcept {
  const std::uint64_t counter = g_incarnation_counter.fetch_add(1) + 1U;
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  std::uint64_t h = 0x1C0FFEEULL;
  h = hash_combine(h, process_id());
  h = hash_combine(h, counter);
  h = hash_combine(h, nanos);
  return IncarnationId::from_raw(h == IncarnationId::kInvalid ? h ^ 0x5DEECE66DULL : h);
}

void sleep_millis(std::uint64_t millis) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_), pid_(other.pid_) {
  other.handle_ = nullptr;
  other.pid_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    pid_ = other.pid_;
    other.handle_ = nullptr;
    other.pid_ = 0;
  }
  return *this;
}

ChildProcess::~ChildProcess() { release(); }

void ChildProcess::release() noexcept {
  if (handle_ != nullptr) {
    static_cast<void>(kill_hard());
    static_cast<void>(wait());
    handle_ = nullptr;
    pid_ = 0;
  }
}

#if defined(_WIN32)

Status ChildProcess::spawn(const std::string& executable,
                           const std::vector<std::string>& arguments, ChildProcess& out) {
  std::string command = "\"" + executable + "\"";
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command.push_back('"');
    command.append(argument);
    command.push_back('"');
  }
  const int wide_length =
      ::MultiByteToWideChar(CP_UTF8, 0, command.c_str(), static_cast<int>(command.size()), nullptr, 0);
  if (wide_length <= 0) {
    return Status::failure(StatusCode::kInvalidArgument, "child command line is not valid UTF-8");
  }
  std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
  static_cast<void>(::MultiByteToWideChar(CP_UTF8, 0, command.c_str(),
                                          static_cast<int>(command.size()), wide.data(), wide_length));

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  const BOOL created =
      ::CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &info);
  if (created == FALSE) {
    return Status::failure(StatusCode::kIo, "child process could not be created");
  }
  ::CloseHandle(info.hThread);
  out.release();
  out.handle_ = info.hProcess;
  out.pid_ = info.dwProcessId;
  return Status{};
}

bool ChildProcess::kill_hard() noexcept {
  if (handle_ == nullptr) {
    return false;
  }
  return ::TerminateProcess(static_cast<HANDLE>(handle_), 1) != FALSE;
}

int ChildProcess::wait() noexcept {
  if (handle_ == nullptr) {
    return -1;
  }
  const DWORD result = ::WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  if (result != WAIT_OBJECT_0) {
    return -1;
  }
  DWORD code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(handle_), &code) == FALSE) {
    return -1;
  }
  ::CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
  return static_cast<int>(code);
}

#else

Status ChildProcess::spawn(const std::string& executable,
                           const std::vector<std::string>& arguments, ChildProcess& out) {
  std::vector<std::string> storage;
  storage.reserve(arguments.size() + 1);
  storage.push_back(executable);
  for (const std::string& argument : arguments) {
    storage.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& entry : storage) {
    argv.push_back(entry.data());
  }
  argv.push_back(nullptr);

  pid_t pid = 0;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  const int result = ::posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (result != 0) {
    return Status::failure(StatusCode::kIo, "child process could not be created");
  }
  out.release();
  out.handle_ = reinterpret_cast<void*>(static_cast<std::uintptr_t>(pid));
  out.pid_ = static_cast<std::uint64_t>(pid);
  return Status{};
}

bool ChildProcess::kill_hard() noexcept {
  if (handle_ == nullptr) {
    return false;
  }
  const auto pid = static_cast<pid_t>(reinterpret_cast<std::uintptr_t>(handle_));
  return ::kill(pid, SIGKILL) == 0;
}

int ChildProcess::wait() noexcept {
  if (handle_ == nullptr) {
    return -1;
  }
  const auto pid = static_cast<pid_t>(reinterpret_cast<std::uintptr_t>(handle_));
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  handle_ = nullptr;
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
}

#endif

}  // namespace mbg::platform
