FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ARG ESP_IDF_REF=v5.5.1
ENV IDF_TOOLS_PATH=/opt/espressif

RUN apt-get update && apt-get install -y \
    git make cmake ninja-build ccache rsync \
    python3 python3-venv python3-pip \
    libffi-dev libssl-dev libpng-dev \
    dfu-util libusb-1.0-0 libsdl2-2.0-0 libslirp0 \
    ca-certificates curl imagemagick && \
    rm -rf /var/lib/apt/lists/*

# Local Docker runs use host uid:gid (typically 1001:1001).
# Add a matching passwd entry to avoid getpwuid warnings in ESP-IDF tooling.
RUN groupadd -g 1001 builder && useradd -m -u 1001 -g 1001 -s /bin/bash builder

# Prebaked toolchain root
RUN mkdir -p /opt/toolchains "$IDF_TOOLS_PATH"

# Pin and install the ESP-IDF baseline. MicroPython is intentionally NOT baked
# in: it is supplied at build time from the deps/micropython/upstream submodule
# (the real version pin), and mpy-cross is built there during the firmware build.
RUN git clone -b ${ESP_IDF_REF} --recursive https://github.com/espressif/esp-idf.git /opt/toolchains/esp-idf && \
    cd /opt/toolchains/esp-idf && \
    export IDF_TOOLS_PATH=/opt/espressif && \
    ./install.sh esp32s3 && \
    . ./export.sh && \
    python3 tools/idf_tools.py install riscv32-esp-elf-gdb

# Validate ESP-IDF environment in-image
RUN bash -lc 'source /opt/toolchains/esp-idf/export.sh >/dev/null 2>&1 && idf.py --version >/dev/null 2>&1'

ENV IDF_PATH=/opt/toolchains/esp-idf

WORKDIR /workspace
