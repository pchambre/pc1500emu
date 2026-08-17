add_library(pc1500bus STATIC
  bus.cpp
  expansion_mock.cpp
)
target_include_directories(pc1500bus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pc1500bus PUBLIC lh5801cpu pc1500keyboard)
