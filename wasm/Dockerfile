FROM emscripten/emsdk:3.1.51

# Build LibreSSL for both native (for configure) and WASM (for linking)
ARG LIBRESSL_VERSION=3.8.2
RUN cd /tmp && \
    curl -sL https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/libressl-${LIBRESSL_VERSION}.tar.gz | tar xz && \
    # Native build (needed for SQLCipher configure step)
    cd /tmp/libressl-${LIBRESSL_VERSION} && \
    mkdir build-native && cd build-native && \
    cmake .. -DCMAKE_INSTALL_PREFIX=/opt/libressl-native \
        -DLIBRESSL_APPS=OFF -DLIBRESSL_TESTS=OFF -DBUILD_SHARED_LIBS=OFF && \
    make -j$(nproc) crypto && make install && \
    # WASM build
    cd /tmp/libressl-${LIBRESSL_VERSION} && \
    mkdir build-wasm && cd build-wasm && \
    emcmake cmake .. -DCMAKE_INSTALL_PREFIX=/opt/libressl-wasm \
        -DCMAKE_C_FLAGS="-O2" \
        -DLIBRESSL_APPS=OFF -DLIBRESSL_TESTS=OFF -DBUILD_SHARED_LIBS=OFF && \
    emmake make -j$(nproc) crypto && emmake make install && \
    rm -rf /tmp/libressl-${LIBRESSL_VERSION}

# Install build tools for SQLCipher configure
RUN apt-get update && apt-get install -y tcl tclsh && rm -rf /var/lib/apt/lists/*

WORKDIR /build
