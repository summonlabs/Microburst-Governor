// Microburst Governor - real operating system process control.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mbg/core/status.hpp"
#include "mbg/model/ids.hpp"

namespace mbg::platform {

/// Identity of the current operating system process.
[[nodiscard]] std::uint64_t process_id() noexcept;

/// Mints a process incarnation identity from the process id, a process local counter and a monotonic
/// clock reading. Uniqueness is what matters here, not ordering: the incarnation never participates
/// in authoritative ordering.
[[nodiscard]] IncarnationId mint_incarnation() noexcept;

/// Sleeps for the requested number of milliseconds. Used only by tools and tests, never by the
/// runtime decision path.
void sleep_millis(std::uint64_t millis) noexcept;

/// A spawned child process. The destructor hard kills a still running child so a failed test cannot
/// leave orphans behind.
class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess();

  [[nodiscard]] static Status spawn(const std::string& executable,
                                    const std::vector<std::string>& arguments,
                                    ChildProcess& out);

  /// Terminates the child immediately and unconditionally. This is a hard kill, not a request.
  [[nodiscard]] bool kill_hard() noexcept;

  /// Blocks until the child exits and reports its exit code. Returns -1 when it cannot be observed.
  [[nodiscard]] int wait() noexcept;

  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
  [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }

 private:
  void release() noexcept;
  void* handle_{nullptr};
  std::uint64_t pid_{0};
};

}  // namespace mbg::platform
