add_executable(lh5801_test lh5801_test.cpp)
target_link_libraries(lh5801_test PRIVATE lh5801cpu)
add_test(NAME lh5801_test COMMAND lh5801_test)

add_executable(bus_test bus_test.cpp)
target_link_libraries(bus_test PRIVATE pc1500bus pc1500keyboard)
add_test(NAME bus_test COMMAND bus_test)

add_executable(lcd_test lcd_test.cpp)
target_link_libraries(lcd_test PRIVATE pc1500bus pc1500keyboard pc1500lcd)
add_test(NAME lcd_test COMMAND lcd_test)
