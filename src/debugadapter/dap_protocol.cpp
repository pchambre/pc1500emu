// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "dap_protocol.h"

#include <string>

namespace pc1500::dap {

std::optional<Json> readMessage(std::istream& in) {
  // Header section: one "Key: Value\r\n" line per header, terminated by a
  // blank line. DAP only defines Content-Length as meaningful; any other
  // header is accepted and ignored per spec.
  long contentLength = -1;
  std::string line;
  while (true) {
    if (!std::getline(in, line)) return std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;  // blank line ends the header section
    const std::string kPrefix = "Content-Length:";
    if (line.compare(0, kPrefix.size(), kPrefix) == 0) {
      contentLength = std::stol(line.substr(kPrefix.size()));
    }
  }
  if (contentLength < 0) return std::nullopt;

  std::string body(static_cast<size_t>(contentLength), '\0');
  in.read(body.data(), contentLength);
  if (in.gcount() != contentLength) return std::nullopt;

  return Json::parse(body, /*cb=*/nullptr, /*allow_exceptions=*/false);
}

void writeMessage(std::ostream& out, const Json& msg) {
  std::string body = msg.dump();
  out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
  out.flush();
}

}  // namespace pc1500::dap
