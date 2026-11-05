# Cross-compile toolchain: Miyoo Mini / Mini Plus (ARM Cortex-A7, OnionOS).
#
# Usage, inside the union-miyoomini-toolchain container:
#
#   cmake -S . -B build_miyoo -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/miyoomini-armhf.cmake \
#     -DGE_VIDEO=ON \
#     -DSDL2_MIYOO_ROOT=$HOME/miyoo-sdl2-prefix
#
# The container image installs the toolchain at /opt/miyoomini-toolchain and
# exports CROSS_COMPILE/PATH for it (support/setup-env.sh). MIYOO_TOOLCHAIN_ROOT
# is a cache variable so a different install prefix needs no edit here.
#
# What this file deliberately does NOT do:
#   - It does not put the Miyoo's own shared libraries on the link line. SDL2,
#     libmi_*, libEGL and libGLESv2 are resolved at runtime from the folder the
#     binary lives in (RPATH $ORIGIN, set in CMakeLists.txt) because that is how
#     OnionOS ships them.
#   - It does not set -m32. ARMv7 is 32-bit by definition, and the engine's
#     32-bit/native-64 split is GE_NATIVE_64, which stays OFF here.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(MIYOO_TOOLCHAIN_ROOT "/opt/miyoomini-toolchain"
    CACHE PATH "union-miyoomini-toolchain install prefix")

set(MIYOO_CROSS "${MIYOO_TOOLCHAIN_ROOT}/usr/bin/arm-linux-gnueabihf-")

set(CMAKE_C_COMPILER   "${MIYOO_CROSS}gcc")
set(CMAKE_CXX_COMPILER "${MIYOO_CROSS}g++")
set(CMAKE_OBJCOPY      "${MIYOO_CROSS}objcopy")
set(CMAKE_AR           "${MIYOO_CROSS}ar")
set(CMAKE_RANLIB       "${MIYOO_CROSS}ranlib")
set(CMAKE_STRIP        "${MIYOO_CROSS}strip")
set(CMAKE_SIZE         "${MIYOO_CROSS}size")

set(CMAKE_SYSROOT "${MIYOO_TOOLCHAIN_ROOT}/usr/arm-linux-gnueabihf/sysroot")

# Cortex-A7 with NEON and VFPv4, hard-float: the ABI the union toolchain, the
# steward-fu SDL2 fork and every OnionOS binary are built for. A soft-float or
# float-abi mismatch here produces a binary that links and then dies on the
# first call through a floating-point argument.
set(MIYOO_ARCH_FLAGS "-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT   "${MIYOO_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${MIYOO_ARCH_FLAGS}")

# Find host tools (Ninja, Python for the build stamp) on the host; find
# libraries and headers only inside the cross sysroot.
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
