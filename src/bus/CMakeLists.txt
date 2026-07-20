add_library(pc1500bus STATIC
  bus.cpp
)
target_include_directories(pc1500bus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
