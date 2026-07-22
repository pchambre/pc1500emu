#include <cstdio>

#include "keyboard.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                        \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);       \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

void testNoKeysPressedReadsAllHigh() {
  pc1500::Keyboard kb;
  for (int col = 0; col < 8; col++) {
    uint8_t strobe = static_cast<uint8_t>(~(1 << col));  // only this column active
    CHECK(kb.scan(strobe) == 0xFF);
  }
  CHECK(kb.scan(0x00) == 0xFF);  // all columns active, still nothing pressed
}

void testRowColPrimitiveMatchesStrobedColumn() {
  pc1500::Keyboard kb;
  // "2" lives at IN0/PA0 per the confirmed matrix.
  kb.setKeyState(/*row=*/0, /*col=*/0, true);

  uint8_t strobeCol0 = static_cast<uint8_t>(~0x01);  // PA0 active-low
  CHECK(kb.scan(strobeCol0) == static_cast<uint8_t>(~0x01));  // IN0 low

  uint8_t strobeCol1 = static_cast<uint8_t>(~0x02);  // PA1 active, PA0 not
  CHECK(kb.scan(strobeCol1) == 0xFF);  // key on PA0 doesn't show up here

  kb.setKeyState(0, 0, false);
  CHECK(kb.scan(strobeCol0) == 0xFF);  // released
}

void testNamedKeyLookupMatchesMatrixPosition() {
  pc1500::Keyboard kb;
  // Z is at IN6/PA6 (confirmed on real hardware; originally misread as
  // "/" during manual transcription -- see hardware reference doc).
  kb.setKeyState(pc1500::Key::Z, true);

  uint8_t strobeCol6 = static_cast<uint8_t>(~(1 << 6));
  uint8_t result = kb.scan(strobeCol6);
  CHECK(((result >> 6) & 1) == 0);  // IN6 pulled low
  CHECK(result == static_cast<uint8_t>(~(1 << 6)));  // no other row affected

  // Same physical key, different column strobed: should not appear.
  uint8_t strobeCol0 = static_cast<uint8_t>(~0x01);
  CHECK(kb.scan(strobeCol0) == 0xFF);
}

void testMultipleSimultaneousColumnsEachContributeIndependently() {
  pc1500::Keyboard kb;
  kb.setKeyState(pc1500::Key::A, true);  // IN3/PA6
  kb.setKeyState(pc1500::Key::Q, true);  // IN5/PA6 -- same column as A
  kb.setKeyState(pc1500::Key::Space, true);  // IN7/PA5 -- different column

  uint8_t strobeCol5And6 = static_cast<uint8_t>(~((1 << 5) | (1 << 6)));
  uint8_t result = kb.scan(strobeCol5And6);
  CHECK(((result >> 3) & 1) == 0);  // A's row (IN3) low
  CHECK(((result >> 5) & 1) == 0);  // Q's row (IN5) low
  CHECK(((result >> 7) & 1) == 0);  // Space's row (IN7) low
  // No other rows should be affected.
  uint8_t expected = static_cast<uint8_t>(~((1 << 3) | (1 << 5) | (1 << 7)));
  CHECK(result == expected);
}

}  // namespace

int main() {
  testNoKeysPressedReadsAllHigh();
  testRowColPrimitiveMatchesStrobedColumn();
  testNamedKeyLookupMatchesMatrixPosition();
  testMultipleSimultaneousColumnsEachContributeIndependently();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
