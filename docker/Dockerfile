# Build environment for the simulator, the thermostat and the tests: the same
# tools locally (through scripts/dev.sh) and in CI.
FROM ubuntu:24.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        git \
        libsdl2-dev \
        ninja-build \
        pkg-config \
        python3 \
        python3-cryptography \
        python3-fonttools \
        python3-pytest \
        python3-zeroconf \
        qemu-user \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Cross-compiler for the thermostat's ARMv7 processor, from Bootlin
# (toolchains.bootlin.com): GCC 15.3, binutils 2.45.1, musl 1.2.6. The thermostat
# runs a 2011-era C library, so Nesto links everything statically against musl
# and needs nothing from the device. The checksum is the one Bootlin publishes
# next to the tarball; update both together.
ARG BOOTLIN_TOOLCHAIN=armv7-eabihf--musl--stable-2026.08-1
ARG BOOTLIN_SHA256=9147bafae4aa272321a3c6440d04d83b7e23411b2d344f875541d84c4444ba9b
RUN curl -fsSL -o /tmp/toolchain.tar.xz \
        "https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/${BOOTLIN_TOOLCHAIN}.tar.xz" \
    && echo "${BOOTLIN_SHA256}  /tmp/toolchain.tar.xz" | sha256sum -c - \
    && mkdir -p /opt/arm-toolchain \
    && tar -xJf /tmp/toolchain.tar.xz -C /opt/arm-toolchain --strip-components=1 \
    && rm /tmp/toolchain.tar.xz \
    && /opt/arm-toolchain/relocate-sdk.sh
ENV NESTO_ARM_TOOLCHAIN=/opt/arm-toolchain

WORKDIR /src
