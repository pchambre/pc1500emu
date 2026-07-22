find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(pc1500emu
  main.cpp
)
target_include_directories(pc1500emu PRIVATE ${SDL2_INCLUDE_DIRS})
target_compile_options(pc1500emu PRIVATE ${SDL2_CFLAGS_OTHER})
target_link_libraries(pc1500emu PRIVATE lh5801cpu pc1500bus pc1500keyboard pc1500lcd ${SDL2_LIBRARIES})
