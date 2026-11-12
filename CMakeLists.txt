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
  ${FW}/common/drivers/fdcan.c
  ${FW}/common/drivers/frame_queue.c
  ${FW}/common/drivers/systick.c
  ${FW}/common/drivers/uart.c
  ${FW}/common/sched/scheduler.c
  ${FW}/common/comm/comm.c
  ${FW}/common/util/crc16.c
  ${FW}/common/diag/diag.c
  ${FW}/common/system_table.c
  ${FW}/common/params/params.c
  ${FW}/common/params/param_source.c
)

# Builds one image from the shared source tree.
function(add_image target id app_source)
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
  # libm for sinf() in the simulated sensors (params/param_source.c); pure maths, no syscalls.
  target_link_libraries(${target} PRIVATE m)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${ARM_SIZE} $<TARGET_FILE:${target}>)
endfunction()

# MCU1 also runs the UART link: monitor/ encodes Protobuf with nanopb (generated code in
# monitor.pb.c, runtime in third_party/nanopb).
add_image(node1 1 ${FW}/nodes/master/app_master.c
  ${FW}/common/monitor/monitor.c
  ${FW}/common/monitor/monitor.pb.c
  ${FW}/common/monitor/cobs.c
  ${FW}/common/drivers/uart_link.c
  ${FW}/common/drivers/tx_slots.c
  ${FW}/third_party/nanopb/pb_common.c
  ${FW}/third_party/nanopb/pb_encode.c)
target_include_directories(node1 SYSTEM PRIVATE ${FW}/third_party/nanopb)
add_image(node2 2 ${FW}/nodes/slave/app_slave.c)
add_image(node3 3 ${FW}/nodes/slave/app_slave.c)
add_image(node4 4 ${FW}/nodes/slave/app_slave.c)

# Self test, not a product image.
add_image(node_loopback 1 ${FW}/tests/app_loopback.c)
