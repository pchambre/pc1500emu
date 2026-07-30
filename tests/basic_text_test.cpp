#include <cstdio>
#include <vector>

#include "basic_text.h"
#include "basic_tokens.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                  \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);   \
      g_failures++;                                                   \
    }                                                                 \
  } while (0)

using pc1500::basic::detokenizeBasicProgram;
using pc1500::basic::keywordForCode;

// PC-1500 Technical Reference Manual section 5-3-5's own worked example:
// "10 PRINT A" / "20 END", stored (without module) at 40C5H-40D2H.
// PRINT = F097H (section 5-2), END = F18EH.
void testManualWorkedExample() {
  std::vector<uint8_t> bytes = {
      0x00, 0x0A, 0x04, 0xF0, 0x97, 'A', 0x0D,  // line 10: PRINT A
      0x00, 0x14, 0x03, 0xF1, 0x8E, 0x0D,       // line 20: END
      0xFF,
  };
  std::string text, error;
  CHECK(detokenizeBasicProgram(bytes, &text, &error));
  CHECK(text == "10 PRINT A\n20 END\n");
}

// Round-trip-shaped smoke test for a few keywords used back-to-back with
// no separating punctuation in the byte stream (the detokenizer must
// still space them apart, since nothing else marks the boundary).
void testAdjacentKeywordsGetSpaced() {
  CHECK(keywordForCode(0xF1A5) != nullptr);  // FOR
  CHECK(keywordForCode(0xF1B1) != nullptr);  // TO

  auto append2 = [](std::vector<uint8_t>& v, uint16_t code) {
    v.push_back(static_cast<uint8_t>(code >> 8));
    v.push_back(static_cast<uint8_t>(code));
  };
  std::vector<uint8_t> content;
  append2(content, 0xF1A5);  // FOR
  content.push_back('I');
  content.push_back('=');
  content.push_back('0');
  append2(content, 0xF1B1);  // TO
  content.push_back('1');
  content.push_back('3');
  content.push_back(0x0D);

  std::vector<uint8_t> bytes = {0x00, 0x01, static_cast<uint8_t>(content.size())};
  bytes.insert(bytes.end(), content.begin(), content.end());
  bytes.push_back(0xFF);

  std::string text, error;
  CHECK(detokenizeBasicProgram(bytes, &text, &error));
  CHECK(text == "1 FOR I=0 TO 13\n");
}

// Bathyscaph listing's label idiom: a bare string-literal statement used
// as a GOSUB/GOTO target name (http://pc1500.com/bathyscaph.html, lines
// 65/70: "IF (QAND G)>0GOSUB \"CRASH\"" / "\"CRASH\"FOR I=0TO 30:..."),
// plus a DATA line whose payload is itself all-digit text inside quotes.
// Confirms quoted spans pass through byte-for-byte with no keyword
// interpretation or added spacing.
void testQuotedLabelAndDataPassThroughVerbatim() {
  auto append2 = [](std::vector<uint8_t>& v, uint16_t code) {
    v.push_back(static_cast<uint8_t>(code >> 8));
    v.push_back(static_cast<uint8_t>(code));
  };
  auto appendStr = [](std::vector<uint8_t>& v, const std::string& s) {
    v.insert(v.end(), s.begin(), s.end());
  };

  // 100 GOSUB "CRASH"
  std::vector<uint8_t> line1;
  append2(line1, 0xF194);  // GOSUB
  appendStr(line1, "\"CRASH\"");
  line1.push_back(0x0D);

  // 1000 DATA "7163470F1F0F47637160"
  std::vector<uint8_t> line2;
  append2(line2, 0xF18D);  // DATA
  appendStr(line2, "\"7163470F1F0F47637160\"");
  line2.push_back(0x0D);

  std::vector<uint8_t> bytes;
  bytes.push_back(0x00);
  bytes.push_back(100);
  bytes.push_back(static_cast<uint8_t>(line1.size()));
  bytes.insert(bytes.end(), line1.begin(), line1.end());
  bytes.push_back(0x03);
  bytes.push_back(0xE8);  // 1000
  bytes.push_back(static_cast<uint8_t>(line2.size()));
  bytes.insert(bytes.end(), line2.begin(), line2.end());
  bytes.push_back(0xFF);

  std::string text, error;
  CHECK(detokenizeBasicProgram(bytes, &text, &error));
  CHECK(text == "100 GOSUB \"CRASH\"\n1000 DATA \"7163470F1F0F47637160\"\n");
}

void testUnrecognizedTokenIsHexEscaped() {
  std::vector<uint8_t> bytes = {
      0x00, 0x01, 0x03, 0xF0, 0x00, 0x0D,  // F000H isn't in the table
      0xFF,
  };
  std::string text, error;
  CHECK(detokenizeBasicProgram(bytes, &text, &error));
  CHECK(text == "1 [F000]\n");
}

void testMissingTerminatorIsAnError() {
  std::vector<uint8_t> bytes = {0x00, 0x0A, 0x04, 0xF0, 0x97, 'A', 0x0D};  // no trailing 0xFF
  std::string text, error;
  CHECK(!detokenizeBasicProgram(bytes, &text, &error));
  CHECK(!error.empty());
}

}  // namespace

int main() {
  testManualWorkedExample();
  testAdjacentKeywordsGetSpaced();
  testQuotedLabelAndDataPassThroughVerbatim();
  testUnrecognizedTokenIsHexEscaped();
  testMissingTerminatorIsAnError();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
