add_custom_command(OUTPUT exports.c
  COMMAND psp-build-exports -b ${CMAKE_CURRENT_SOURCE_DIR}/exports.exp > exports.c
  DEPENDS exports.exp WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_executable(pkgi_home_module main.c exports.c)
target_compile_options(pkgi_home_module PRIVATE -Os -G0 -Wall)
target_link_options(pkgi_home_module PRIVATE -nostartfiles -Wl,-q
  -Wl,-T$ENV{PSPDEV}/psp/sdk/lib/linkfile.prx -Wl,-zmax-page-size=128)
target_link_libraries(pkgi_home_module PRIVATE pspctrl_driver pspsdk pspkernel)
add_custom_command(OUTPUT pkgi_home.prx
  COMMAND psp-prxgen $<TARGET_FILE:pkgi_home_module> pkgi_home.prx
  COMMAND psp-fixup-imports pkgi_home.prx
  DEPENDS pkgi_home_module WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_custom_command(OUTPUT pkgi_home.S
  COMMAND psp-build-exports -s ${CMAKE_CURRENT_SOURCE_DIR}/exports.exp
  DEPENDS exports.exp WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_custom_command(OUTPUT home_blob.o
  COMMAND ${CMAKE_LINKER} -r -b binary -o home_blob.o pkgi_home.prx
  DEPENDS pkgi_home.prx WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_library(pkgi_home_bridge STATIC ${CMAKE_CURRENT_BINARY_DIR}/pkgi_home.S ${CMAKE_CURRENT_BINARY_DIR}/home_blob.o)


