add_executable(pc1500sim sim.cpp)
target_link_libraries(pc1500sim PRIVATE lh5801cpu)
