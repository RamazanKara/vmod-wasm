# Third-Party Notices

This project is released under the BSD-2-Clause license. Release tarballs may
include third-party components with their own licenses.

## Bundled Release Runtime

Binary release bundles include `libwasmtime.so` from the Wasmtime C API
distribution.

- Project: Wasmtime
- Upstream: https://github.com/bytecodealliance/wasmtime
- License: Apache-2.0 WITH LLVM-exception
- Bundled version: 44.0.0

## Build-Time And Example Dependencies

The Rust example modules are built from the `examples/` Cargo workspace. Their
resolved dependency versions are recorded in `examples/Cargo.lock`.

Notable dependencies include:

- `proxy-wasm`, used by Proxy-Wasm SDK examples.
- `serde` and `serde-json-wasm`, used by the edge security filter example.

Downstream redistributors should review the license metadata for dependencies
resolved in `examples/Cargo.lock` when shipping rebuilt example `.wasm`
artifacts.

## Imported Source Attribution

`src/compat.h` carries its own BSD-2-Clause copyright notice:

- Copyright (c) 2024 OTTO GmbH & Co KG
