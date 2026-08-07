// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include <cstdio>
#include <sstream>
#include <string>

#include "dap_protocol.h"

namespace {

using namespace pc1500::dap;

int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      g_failures++;                                                 \
    }                                                                \
  } while (0)

void testWriteThenReadRoundTrip() {
  Json request = {{"seq", 1}, {"type", "request"}, {"command", "initialize"}};
  std::ostringstream out;
  writeMessage(out, request);

  std::string framed = out.str();
  CHECK(framed.find("Content-Length: ") == 0);
  CHECK(framed.find("\r\n\r\n") != std::string::npos);

  std::istringstream in(framed);
  auto parsed = readMessage(in);
  CHECK(parsed.has_value());
  if (parsed) {
    CHECK((*parsed)["seq"] == 1);
    CHECK((*parsed)["command"] == "initialize");
  }
}

void testTwoMessagesBackToBack() {
  std::ostringstream out;
  writeMessage(out, Json{{"seq", 1}});
  writeMessage(out, Json{{"seq", 2}});

  std::istringstream in(out.str());
  auto first = readMessage(in);
  auto second = readMessage(in);
  CHECK(first.has_value() && (*first)["seq"] == 1);
  CHECK(second.has_value() && (*second)["seq"] == 2);
}

void testEofReturnsNullopt() {
  std::istringstream in("");
  CHECK(!readMessage(in).has_value());
}

void testUnicodeBodySurvivesRoundTrip() {
  Json msg = {{"text", "line → with é chars"}};
  std::ostringstream out;
  writeMessage(out, msg);
  std::istringstream in(out.str());
  auto parsed = readMessage(in);
  CHECK(parsed.has_value());
  if (parsed) CHECK((*parsed)["text"] == msg["text"]);
}

}  // namespace

int main() {
  testWriteThenReadRoundTrip();
  testTwoMessagesBackToBack();
  testEofReturnsNullopt();
  testUnicodeBodySurvivesRoundTrip();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
