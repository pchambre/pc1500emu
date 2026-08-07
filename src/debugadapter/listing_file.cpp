// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "listing_file.h"

#include <fstream>
#include <regex>
#include <sstream>

namespace pc1500::dap {

namespace {

// One data-emitting listing line: <4-hex address> <hex bytes...>
// <decimal source line number> <TAB> <source text>. Matches the format
// sdcc-pc1500's own test_lh5801.py parses from sdaslh5801 -l output
// (confirmed against that script this session); lines with no emitted
// bytes (blank lines, comments, zero-width directives) don't match this
// pattern and are simply skipped -- they can't host a breakpoint anyway.
const std::regex kListingLineRe(
    R"(^\s*([0-9A-Fa-f]{4})\s+((?:[0-9A-Fa-f]{2}\s*)+?)\s+(\d+)\s+\t(\S.*)$)");

}  // namespace

bool ListingFile::load(const std::string& path, std::string* error) {
  std::ifstream f(path);
  if (!f) {
    if (error) *error = "cannot open listing file: " + path;
    return false;
  }

  lines_.clear();
  std::string raw;
  while (std::getline(f, raw)) {
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    std::smatch m;
    if (!std::regex_match(raw, m, kListingLineRe)) continue;
    ListingLine line;
    line.address = static_cast<uint16_t>(std::stoul(m[1].str(), nullptr, 16));
    line.sourceLine = std::stoi(m[3].str());
    line.sourceText = m[4].str();
    lines_.push_back(std::move(line));
  }

  if (lines_.empty()) {
    if (error) *error = "no listing lines recognized in: " + path;
    return false;
  }
  return true;
}

std::optional<uint16_t> ListingFile::addressForLine(int sourceLine) const {
  for (const auto& l : lines_) {
    if (l.sourceLine == sourceLine) return l.address;
  }
  return std::nullopt;
}

std::optional<int> ListingFile::lineForAddress(uint16_t address) const {
  for (const auto& l : lines_) {
    if (l.address == address) return l.sourceLine;
  }
  return std::nullopt;
}

}  // namespace pc1500::dap
