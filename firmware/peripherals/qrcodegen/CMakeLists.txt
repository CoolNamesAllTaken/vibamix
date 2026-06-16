target_sources(app PRIVATE
  qrcodegen.c
)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
