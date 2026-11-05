# Build image for the Miyoo cross build's dependencies.
#
# Extends shauninman's union-miyoomini-toolchain image with what the SDL2 fork's
# configure script and this project's CMakeLists.txt need, and which the
# upstream image does not provide:
#
#   - autoconf/automake/libtool: sdl2/sdl2/autogen.sh fails with
#     "autoconf: not found" without them.
#   - CMake >= 3.16: the image ships 3.13.4, and CMakeLists.txt's first line
#     requires 3.16. Kitware's prebuilt tarball is used rather than pip because
#     the base image's pip (18.1, on Python 3.7) predates the manylinux2014 tags
#     and cannot install a modern cmake wheel.
#   - json-c is built from sdl2/sdl2/dependency/ by build_miyoo_sdl2.sh.
#
#   docker build -t miyoo-build -f scripts/port/miyoo-build.Dockerfile .
#
# The base image is produced by:
#   docker build -t miyoomini-toolchain ./union-miyoomini-toolchain
FROM miyoomini-toolchain

# debian/eol:buster-slim already points at archive.debian.org, so this resolves;
# buster is frozen, so these versions are stable.
RUN apt-get -y update && apt-get -y install --no-install-recommends \
        autoconf \
        automake \
        libtool \
        pkg-config \
        file \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

# CMake 3.27.9, arch-matched: the image is x86_64 on an Intel host and arm64 on
# Apple Silicon, and the same Dockerfile has to work for both.
ARG CMAKE_VERSION=3.27.9
RUN set -eux; \
    case "$(uname -m)" in \
        x86_64)  ARCH=x86_64 ;; \
        aarch64) ARCH=aarch64 ;; \
        *) echo "unsupported build arch: $(uname -m)" >&2; exit 1 ;; \
    esac; \
    wget -q -O /tmp/cmake.tar.gz \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${ARCH}.tar.gz"; \
    mkdir -p /opt/cmake; \
    tar -xzf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1; \
    rm /tmp/cmake.tar.gz; \
    ln -sf /opt/cmake/bin/cmake /usr/local/bin/cmake; \
    ln -sf /opt/cmake/bin/ctest /usr/local/bin/ctest

RUN cmake --version | head -1
