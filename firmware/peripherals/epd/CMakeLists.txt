target_sources(app PRIVATE
  epaper_display/Display_EPD_W21.cpp
  epaper_display/Display_EPD_W21_spi.cpp
  gui/GUI_Paint.cpp
  fonts/font8.cpp
  fonts/font12.cpp
  fonts/font16.cpp
  fonts/font20.cpp
  fonts/font24.cpp
)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/epaper_display
  ${CMAKE_CURRENT_SOURCE_DIR}/gui
  ${CMAKE_CURRENT_SOURCE_DIR}/fonts
)
