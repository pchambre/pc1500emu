# Find SDL2/SDL2_ttf: prefer CMake config-mode targets (vcpkg on Windows),
# fall back to pkg-config (Linux distro packages).
find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
  set(SDL2_TARGETS SDL2::SDL2 $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(SDL2_TARGETS PkgConfig::SDL2)
endif()

find_package(SDL2_ttf CONFIG QUIET)
if(TARGET SDL2_ttf::SDL2_ttf)
  set(SDL2_TTF_TARGETS SDL2_ttf::SDL2_ttf)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
  set(SDL2_TTF_TARGETS PkgConfig::SDL2_TTF)
endif()

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_executable(pc1500emu
  main.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(pc1500emu PRIVATE
  ${IMGUI_DIR} ${IMGUI_DIR}/backends
)
target_link_libraries(pc1500emu PRIVATE lh5801cpu pc1500bus pc1500keyboard pc1500lcd pc1500basic ${SDL2_TARGETS} ${SDL2_TTF_TARGETS})
