add_executable(pc1500emu
  main.cpp
)
target_link_libraries(pc1500emu PRIVATE lh5801cpu pc1500bus pc1500keyboard pc1500lcd)
