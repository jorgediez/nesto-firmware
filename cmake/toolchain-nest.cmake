# Cross-compiles for the Nest Learning Thermostat (2nd gen): a TI AM3703,
# ARM Cortex-A8 with NEON, running Linux 2.6.37.
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-nest.cmake ...
#
# Uses the Bootlin musl toolchain installed in the dev image (see
# docker/Dockerfile). The program is linked fully statically, so it doesn't
# depend on the thermostat's own, much older C library.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(DEFINED ENV{NESTO_ARM_TOOLCHAIN})
  set(NESTO_ARM_TOOLCHAIN "$ENV{NESTO_ARM_TOOLCHAIN}")
else()
  set(NESTO_ARM_TOOLCHAIN /opt/arm-toolchain)
endif()
set(_triple arm-buildroot-linux-musleabihf)

set(CMAKE_C_COMPILER "${NESTO_ARM_TOOLCHAIN}/bin/${_triple}-gcc")
set(CMAKE_CXX_COMPILER "${NESTO_ARM_TOOLCHAIN}/bin/${_triple}-g++")
set(CMAKE_SYSROOT "${NESTO_ARM_TOOLCHAIN}/${_triple}/sysroot")

# The toolchain defaults to a Cortex-A9; tune for the A8 instead, with its NEON
# unit. The A8 lacks the A9's multiprocessing extension, so scripts/check-arm.sh
# also checks the result contains no instructions that need it.
set(_cpu_flags "-mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT "${_cpu_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_cpu_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# Look for libraries and headers only in the toolchain, never on the build
# machine. That includes pkg-config, so the simulator's ARM build doesn't
# pick up the host's SDL2 and stays headless.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")

# ctest runs ARM test programs through QEMU, emulating the Nest's CPU.
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-arm -cpu cortex-a8)
