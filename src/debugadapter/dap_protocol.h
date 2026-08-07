// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// Debug Adapter Protocol transport: Content-Length-framed JSON over a
// stream, per https://microsoft.github.io/debug-adapter-protocol/overview
// ("Base Protocol"). This is the entire transport layer -- request/
// response/event message *shapes* are handled by debug_adapter_main.cpp,
// this file only knows how to frame and unframe one JSON blob at a time.
#pragma once

#include <istream>
#include <optional>
#include <ostream>

#include "json.hpp"

namespace pc1500::dap {

using Json = nlohmann::json;

// Blocks reading from `in` until one full framed message has arrived and
// returns its parsed JSON body, or std::nullopt if the stream hit EOF/an
// error before a complete message could be read (the normal shutdown
// signal when VS Code closes stdin).
std::optional<Json> readMessage(std::istream& in);

// Serializes `msg` and writes it to `out` with the required
// Content-Length header, then flushes -- DAP has no message delimiter
// other than the byte count, so a missed flush would wedge the reader on
// the other end.
void writeMessage(std::ostream& out, const Json& msg);

}  // namespace pc1500::dap
