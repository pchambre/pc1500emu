add_library(lh5801cpu STATIC
  lh5801.cpp
)
target_include_directories(lh5801cpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
