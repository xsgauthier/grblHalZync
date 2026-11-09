add_library(zynq INTERFACE)

target_sources(zynq INTERFACE
 ${CMAKE_CURRENT_LIST_DIR}/driver.c
 ${CMAKE_CURRENT_LIST_DIR}/mcu.c
 ${CMAKE_CURRENT_LIST_DIR}/grbl_interface.c
 ${CMAKE_CURRENT_LIST_DIR}/platform_linux.c
)

target_include_directories(zynq INTERFACE ${CMAKE_CURRENT_LIST_DIR})
