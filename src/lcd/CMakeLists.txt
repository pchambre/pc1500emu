add_library(pc1500lcd STATIC
  lcd.cpp
)
target_include_directories(pc1500lcd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
