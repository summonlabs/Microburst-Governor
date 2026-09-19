// Microburst Governor - command line front end (internal).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mbg::cli {

/// Very small argument reader: flags, valued options and positional tokens.
class Arguments {
 public:
  Arguments() = default;
  explicit Arguments(std::vector<std::string> tokens) : tokens_(std::move(tokens)) {}

  [[nodiscard]] bool has(std::string_view flag) const;
  [[nodiscard]] std::optional<std::string> value(std::string_view flag) const;
  [[nodiscard]] std::optional<std::uint64_t> number(std::string_view flag) const;
  [[nodiscard]] const std::vector<std::string>& tokens() const noexcept { return tokens_; }

 private:
  std::vector<std::string> tokens_{};
};

int run_replay(const Arguments& arguments);
int run_verify(const Arguments& arguments);
int run_bench(const Arguments& arguments);
int run_generate(const Arguments& arguments);
int run_selftest(const Arguments& arguments);

void print_usage();

/// Reads a whole file, bounded. Returns false when the file cannot be read or exceeds the bound.
[[nodiscard]] bool read_file(const std::string& path, std::uint64_t maximum, std::string& out);
[[nodiscard]] bool write_file(const std::string& path, const std::string& contents);

}  // namespace mbg::cli
