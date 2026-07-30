add_executable(lh5801_test lh5801_test.cpp)
target_link_libraries(lh5801_test PRIVATE lh5801cpu)
add_test(NAME lh5801_test COMMAND lh5801_test)

add_executable(lh5801_interrupt_test lh5801_interrupt_test.cpp)
target_link_libraries(lh5801_interrupt_test PRIVATE lh5801cpu)
add_test(NAME lh5801_interrupt_test COMMAND lh5801_interrupt_test)

add_executable(bus_test bus_test.cpp)
target_link_libraries(bus_test PRIVATE pc1500bus pc1500keyboard)
add_test(NAME bus_test COMMAND bus_test)

add_executable(lcd_test lcd_test.cpp)
target_link_libraries(lcd_test PRIVATE pc1500bus pc1500keyboard pc1500lcd)
add_test(NAME lcd_test COMMAND lcd_test)

add_executable(keyboard_test keyboard_test.cpp)
target_link_libraries(keyboard_test PRIVATE pc1500keyboard)
add_test(NAME keyboard_test COMMAND keyboard_test)

add_executable(basic_text_test basic_text_test.cpp)
target_link_libraries(basic_text_test PRIVATE pc1500basic)
add_test(NAME basic_text_test COMMAND basic_text_test)
