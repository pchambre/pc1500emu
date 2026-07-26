find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)

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
  ${SDL2_INCLUDE_DIRS} ${SDL2_TTF_INCLUDE_DIRS}
  ${IMGUI_DIR} ${IMGUI_DIR}/backends
)
target_compile_options(pc1500emu PRIVATE ${SDL2_CFLAGS_OTHER} ${SDL2_TTF_CFLAGS_OTHER})
target_link_libraries(pc1500emu PRIVATE lh5801cpu pc1500bus pc1500keyboard pc1500lcd ${SDL2_LIBRARIES} ${SDL2_TTF_LIBRARIES})
