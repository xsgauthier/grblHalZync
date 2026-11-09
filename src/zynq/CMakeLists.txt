add_library(zynq INTERFACE)

target_sources(zynq INTERFACE
 ${CMAKE_CURRENT_LIST_DIR}/driver.c
)

target_include_directories(zynq INTERFACE ${CMAKE_CURRENT_LIST_DIR})
