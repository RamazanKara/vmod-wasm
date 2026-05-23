FROM varnish:9.0.3

ARG WASMTIME_VERSION=44.0.0

USER root

# Install build dependencies (Varnish 9 is already installed in base image)
RUN apt-get update && apt-get install -y \
    automake \
    autoconf \
    libtool \
    pkg-config \
    gcc \
    make \
    python3 \
    curl \
    ca-certificates \
    xz-utils \
    varnish-dev \
    && rm -rf /var/lib/apt/lists/*

# Install Wasmtime C API
RUN ARCH=$(dpkg --print-architecture) \
    && if [ "$ARCH" = "amd64" ]; then WASMTIME_ARCH="x86_64"; else WASMTIME_ARCH="aarch64"; fi \
    && curl -fsSL "https://github.com/bytecodealliance/wasmtime/releases/download/v${WASMTIME_VERSION}/wasmtime-v${WASMTIME_VERSION}-${WASMTIME_ARCH}-linux-c-api.tar.xz" \
       | tar -xJ -C /opt/ \
    && ln -s /opt/wasmtime-v${WASMTIME_VERSION}-${WASMTIME_ARCH}-linux-c-api /opt/wasmtime

ENV WASMTIME_DIR=/opt/wasmtime
ENV LD_LIBRARY_PATH=/opt/wasmtime/lib

# Install Rust (for building test Wasm modules)
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable \
    && . "$HOME/.cargo/env" \
    && rustup target add wasm32-unknown-unknown

ENV PATH="/root/.cargo/bin:${PATH}"

WORKDIR /src

COPY . .

# Build all Wasm example modules via workspace
RUN cd examples \
    && cargo build --release --target wasm32-unknown-unknown \
    && mkdir -p /src/tests/wasm \
    && cp target/wasm32-unknown-unknown/release/test_module.wasm /src/tests/wasm/ \
    && cp target/wasm32-unknown-unknown/release/proxy_wasm_filter.wasm /src/tests/wasm/ \
    && cp target/wasm32-unknown-unknown/release/passthrough.wasm /src/tests/wasm/ \
    && cp target/wasm32-unknown-unknown/release/transform.wasm /src/tests/wasm/ \
    && cp target/wasm32-unknown-unknown/release/edge_security_filter.wasm /src/tests/wasm/

# Build the VMOD
RUN chmod +x autogen.sh \
    && ./autogen.sh \
    && ./configure --with-wasmtime=${WASMTIME_DIR} \
    && make

CMD ["make", "check"]
