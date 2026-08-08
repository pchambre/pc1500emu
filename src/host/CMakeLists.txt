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

# main.cpp's requestPick runs the (occasionally slow, see
# nativeWinPickPathEarlyEvent's own comment on Windows; std::async wrapping
# tinyfiledialogs elsewhere) native file-picker call on a worker thread so
# it doesn't block this single-threaded app's whole frame loop -- needs
# actual thread support linked in on Linux (glibc's std::thread/std::async
# depend on libpthread on older glibc; harmless to link even where it's
# already folded into libc on newer ones). Windows' MSVC runtime doesn't
# need this, but it's unconditional here since CMake's FindThreads already
# no-ops cleanly there.
find_package(Threads REQUIRED)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(TFD_DIR ${CMAKE_SOURCE_DIR}/third_party/tinyfiledialogs)

add_executable(pc1500emu
  main.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
  ${TFD_DIR}/tinyfiledialogs.c
)
target_include_directories(pc1500emu PRIVATE
  ${IMGUI_DIR} ${IMGUI_DIR}/backends ${TFD_DIR}
)
target_compile_definitions(pc1500emu PRIVATE PC1500EMU_VERSION="${PROJECT_VERSION}")
target_link_libraries(pc1500emu PRIVATE lh5801cpu pc1500bus pc1500keyboard pc1500lcd pc1500basic pc1500hoststate ${SDL2_TARGETS} ${SDL2_TTF_TARGETS} Threads::Threads)
if(WIN32)
  # tinyfiledialogs is still linked in (it's vendored unconditionally,
  # above) but on Windows main.cpp's requestPick no longer calls into it at
  # all -- see nativeWinPickPathEarlyEvent's own comment for why -- so
  # comdlg32/shell32 aren't strictly needed by our own code anymore, only
  # by tinyfiledialogs.c itself; kept linked since it's still compiled in.
  # main.cpp's own nativeWinPickPathEarlyEvent additionally calls
  # CoCreateInstance/CoInitializeEx/CoTaskMemFree (ole32) and creates
  # IFileOpenDialog/IFileSaveDialog (uuid, for the CLSID/IID GUID
  # definitions shobjidl.h only declares extern). None of these are
  # auto-linked by MSVC the way some system libs are, so all are explicit.
  target_link_libraries(pc1500emu PRIVATE comdlg32 shell32 ole32 uuid)
endif()
