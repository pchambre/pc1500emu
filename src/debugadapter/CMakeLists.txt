set(JSON_DIR ${CMAKE_SOURCE_DIR}/third_party/json)

add_library(pc1500debugadapterlib STATIC
  dap_protocol.cpp
  emulator_client.cpp
  listing_file.cpp
)
target_include_directories(pc1500debugadapterlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${JSON_DIR})

add_executable(pc1500debugadapter_cli debug_adapter_main.cpp)
target_link_libraries(pc1500debugadapter_cli PRIVATE pc1500debugadapterlib)
set_target_properties(pc1500debugadapter_cli PROPERTIES OUTPUT_NAME pc1500debugadapter)
