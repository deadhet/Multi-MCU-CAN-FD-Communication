cmake_minimum_required(VERSION 3.21)

if(NOT CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake")
endif()

project(multi_mcu_canfd LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(FW ${CMAKE_SOURCE_DIR}/firmware)
set(LINKER_SCRIPT ${FW}/linker/stm32h743vit6.ld)

# Sources every node links. Later stages add drivers, comm, params and diag here.
set(COMMON_SOURCES
  ${FW}/startup/startup_stm32h743.c
  ${FW}/common/drivers/board.c
  ${FW}/common/drivers/clock.c
  ${FW}/common/drivers/systick.c
  ${FW}/common/drivers/uart.c
  ${FW}/common/sched/scheduler.c
)

# Builds nodeN.elf from one source tree.
function(add_node id app_source)
  set(target node${id})
  add_executable(${target} ${COMMON_SOURCES} ${app_source} ${ARGN})
  set_target_properties(${target} PROPERTIES SUFFIX .elf LINK_DEPENDS ${LINKER_SCRIPT})
  target_compile_definitions(${target} PRIVATE STM32H743xx NODE_ID=${id})
  target_include_directories(${target} PRIVATE ${FW}/common)
  target_include_directories(${target} SYSTEM PRIVATE
    ${FW}/third_party/cmsis_core/Include
    ${FW}/third_party/cmsis_device_h7/Include)
  target_compile_options(${target} PRIVATE
    -O2 -g3 -ffunction-sections -fdata-sections -fno-common
    -Wall -Wextra -Wshadow -Wstrict-prototypes -Werror)
  # No -lnosys: any libc call that needs a syscall, such as malloc, fails at link time.
  target_link_options(${target} PRIVATE
    -T${LINKER_SCRIPT} -nostartfiles --specs=nano.specs
    -Wl,--gc-sections -Wl,--fatal-warnings
    -Wl,-Map=$<TARGET_FILE_DIR:${target}>/${target}.map)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${ARM_SIZE} $<TARGET_FILE:${target}>)
endfunction()

add_node(1 ${FW}/nodes/master/app_master.c ${FW}/common/monitor/monitor.c)
add_node(2 ${FW}/nodes/slave/app_slave.c)
add_node(3 ${FW}/nodes/slave/app_slave.c)
add_node(4 ${FW}/nodes/slave/app_slave.c)
