set(JSON_DIR ${CMAKE_SOURCE_DIR}/third_party/json)

add_library(pc1500hoststate STATIC
  state_file.cpp
  app_config.cpp
)
target_include_directories(pc1500hoststate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${JSON_DIR})
target_link_libraries(pc1500hoststate PUBLIC pc1500bus)
