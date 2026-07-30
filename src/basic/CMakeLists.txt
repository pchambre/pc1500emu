add_library(pc1500basic STATIC
  basic_tokens.cpp
  basic_text.cpp
)
target_include_directories(pc1500basic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
