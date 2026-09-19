// Microburst Governor - command line front end (internal).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "cli_common.hpp"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "mbg/limits.hpp"
#include "mbg/version.hpp"

namespace mbg::cli {

bool Arguments::has(std::string_view flag) const {
  for (const std::string& token : tokens_) {
    if (token == flag) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> Arguments::value(std::string_view flag) const {
  for (std::size_t i = 0; i + 1 < tokens_.size(); ++i) {
    if (tokens_[i] == flag) {
      return tokens_[i + 1];
    }
  }
  return std::nullopt;
}

std::optional<std::uint64_t> Arguments::number(std::string_view flag) const {
  const std::optional<std::string> text = value(flag);
  if (!text.has_value()) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(text->data(), text->data() + text->size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text->data() + text->size()) {
    return std::nullopt;
  }
  return parsed;
}

bool read_file(const std::string& path, std::uint64_t maximum, std::string& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return false;
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > maximum) {
    return false;
  }
  stream.seekg(0, std::ios::beg);
  out.assign(static_cast<std::size_t>(size), '\0');
  stream.read(out.data(), size);
  return stream.good() || stream.eof();
}

bool write_file(const std::string& path, const std::string& contents) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return stream.good();
}

void print_usage() {
  std::puts(
      "mbgctl " MBG_VERSION_STRING " - Microburst Governor tool\n"
      "\n"
      "usage:\n"
      "  mbgctl selftest\n"
      "  mbgctl generate --shape <name> [--ticks N] [--seed N] [--out FILE]\n"
      "  mbgctl replay --trace FILE [--policy-window N] [--interventions 0|1] [--events]\n"
      "  mbgctl verify --store DIR\n"
      "  mbgctl bench [--resources N] [--ticks N] [--window N] [--bursts N] [--seed N]\n"
      "\n"
      "shapes: steady, microburst, ramp, oscillating, noisy-steady, sparse-sampling,\n"
      "        counter-reset, capacity-change, missing-samples, reordered, duplicate,\n"
      "        shallow-noise, long-burst\n");
}

}  // namespace mbg::cli
