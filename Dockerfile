FROM debian:bookworm-slim

ARG WASMTIME_VERSION=44.0.0

# Install build dependencies
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
    gnupg \
    xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Install Varnish 8.0 from packagecloud
RUN curl -fsSL https://packagecloud.io/varnishcache/varnish80/gpgkey \
       | gpg --dearmor -o /usr/share/keyrings/varnish.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/varnish.gpg] https://packagecloud.io/varnishcache/varnish80/debian/ bookworm main" \
       > /etc/apt/sources.list.d/varnish.list \
    && apt-get update \
    && apt-get install -y varnish-dev varnish \
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

# Build the test Wasm module
RUN cd examples/rust && cargo build --release \
    && mkdir -p /src/tests/wasm \
    && cp target/wasm32-unknown-unknown/release/test_module.wasm /src/tests/wasm/

# Build the VMOD
RUN chmod +x autogen.sh \
    && ./autogen.sh \
    && ./configure --with-wasmtime=${WASMTIME_DIR} \
    && make

CMD ["make", "check"]
