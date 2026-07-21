add_executable(lh5801_test lh5801_test.cpp)
target_link_libraries(lh5801_test PRIVATE lh5801cpu)
add_test(NAME lh5801_test COMMAND lh5801_test)
