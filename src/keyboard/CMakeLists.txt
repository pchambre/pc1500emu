add_library(pc1500keyboard STATIC
  keyboard.cpp
)
target_include_directories(pc1500keyboard PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
