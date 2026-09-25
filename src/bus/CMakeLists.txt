add_library(pc1500bus STATIC
  bus.cpp
  expansion_mock.cpp
)
target_include_directories(pc1500bus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pc1500bus PUBLIC lh5801cpu pc1500keyboard)

# The expansion board's keyword executor (2026-09-25) is portable C in the
# PC1500-PSOC5 repo's RP2350/keywords.c -- the mock compiles that same file
# rather than a copy, so the keyword tests exercise the real parser. Point
# PC1500_EXPANSION_FIRMWARE_DIR at that directory; without it the mock
# answers EXP_COMMAND_KEYWORD with NOT_IMPLEMENTED (every keyword of the
# current expansion ROM then raises ERROR 1).
set(PC1500_EXPANSION_FIRMWARE_DIR
    "C:/Users/paulc/Documents/PSoC Creator/PC1500-PSOC5/RP2350"
    CACHE PATH "PC1500-PSOC5 RP2350 firmware directory (keywords.c, pc_exp.h)")
if(EXISTS "${PC1500_EXPANSION_FIRMWARE_DIR}/keywords.c")
  target_sources(pc1500bus PRIVATE "${PC1500_EXPANSION_FIRMWARE_DIR}/keywords.c")
  target_include_directories(pc1500bus PRIVATE "${PC1500_EXPANSION_FIRMWARE_DIR}")
  target_compile_definitions(pc1500bus PRIVATE PC1500_HAVE_EXPANSION_KEYWORDS=1)
else()
  message(WARNING "keywords.c not found in PC1500_EXPANSION_FIRMWARE_DIR "
                  "(${PC1500_EXPANSION_FIRMWARE_DIR}) -- expansion keywords will not work")
endif()
