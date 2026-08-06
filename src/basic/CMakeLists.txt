add_library(pc1500basic STATIC
  basic_tokens.cpp
  basic_text.cpp
  text_loader.cpp
)
target_include_directories(pc1500basic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pc1500basic PUBLIC pc1500bus)
