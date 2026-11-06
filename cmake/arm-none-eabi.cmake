# Toolchain file: Arm GNU Toolchain for the STM32H743 (Cortex-M7, double-precision FPU).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

find_program(ARM_GCC arm-none-eabi-gcc REQUIRED)
find_program(ARM_SIZE arm-none-eabi-size REQUIRED)
set(CMAKE_C_COMPILER ${ARM_GCC})

# Bare-metal: a linked test program cannot be built before the linker script is known.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard")
