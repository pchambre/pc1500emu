add_library(pc1500disasm STATIC
  opcode_table.cpp
  analyzer.cpp
  formatter.cpp
  known_symbols.cpp
  tasm_convert.cpp
)
target_include_directories(pc1500disasm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(pc1500disasm_cli disasm_main.cpp)
target_link_libraries(pc1500disasm_cli PRIVATE pc1500disasm)
set_target_properties(pc1500disasm_cli PROPERTIES OUTPUT_NAME pc1500disasm)
