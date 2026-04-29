FROM debian:bookworm-slim

ARG VARNISH_VERSION=7.5
ARG WASMTIME_VERSION=25.0.0

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
    && rm -rf /var/lib/apt/lists/*

# Install Varnish from official repo
RUN curl -fsSL https://packagecloud.io/varnishcache/varnish75/gpgkey | gpg --dearmor -o /usr/share/keyrings/varnish.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/varnish.gpg] https://packagecloud.io/varnishcache/varnish75/debian/ bookworm main" \
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

WORKDIR /src

COPY . .

RUN chmod +x autogen.sh \
    && ./autogen.sh \
    && ./configure --with-wasmtime=${WASMTIME_DIR} \
    && make

CMD ["make", "check"]
