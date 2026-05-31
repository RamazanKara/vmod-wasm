# Documentation Guide

This directory contains the operator, developer, and design documentation for
vmod-wasm. The project targets Varnish 9.x with Wasmtime 44.0.0, so the docs
assume that release contract unless a section says otherwise.

## Start Here

- Read [Production Deployment](PRODUCTION.md) when deciding how to install,
  constrain, monitor, reload, and roll back vmod-wasm in front of real traffic.
- Read [Development Guide](DEVELOPMENT.md) when writing or adapting a
  Proxy-Wasm module for Varnish.
- Read [Proxy-Wasm Compatibility](COMPATIBILITY.md) before porting an existing
  filter from another Proxy-Wasm host.
- Read [Configuration Reference](CONFIGURATION.md) when you need exact VCL
  function names, return values, defaults, and valid scopes.

## Documents

| Document | Use it for |
|----------|------------|
| [Production Deployment](PRODUCTION.md) | Release bundles, runtime limits, monitoring, VCL reloads, canaries, rollback, capacity planning |
| [Development Guide](DEVELOPMENT.md) | Building Rust/Wasm modules, SDK callback shape, HTTP callouts, tests, debugging |
| [Configuration Reference](CONFIGURATION.md) | Complete `wasm.*` VCL API reference with parameters, return values, and examples |
| [Proxy-Wasm Compatibility](COMPATIBILITY.md) | ABI support matrix, properties, pseudo-headers, limitations |
| [Architecture](ARCHITECTURE.md) | Engine lifetime, store pooling, HTTP callout flow, VDP response-body integration, file map |
| [Security Model](SECURITY.md) | Sandbox boundaries, SSRF controls, module supply-chain guidance, incident response |

## Example Modules

The [`examples/`](../examples/) workspace contains small, focused modules that
exercise the VMOD from VTC tests:

- [`proxy-wasm-filter`](../examples/proxy-wasm-filter/) is the best starting
  point for a normal Rust `proxy-wasm` SDK module.
- [`passthrough`](../examples/passthrough/) is a no-op lifecycle baseline.
- [`transform`](../examples/transform/) shows a minimal response-header
  mutation.
- [`rust`](../examples/rust/) demonstrates raw vmod-wasm host functions.
- [`edge-security-filter`](../examples/edge-security-filter/) is a realistic
  reference fixture covering config, metrics, shared data, callouts, body
  callbacks, and local responses.

For production edge-security deployments, use the standalone
[vmod-wasm Edge Security Filter](https://github.com/RamazanKara/vmod-wasm-edge-security-filter)
repository. The in-tree filter remains a fixture so vmod-wasm can test a
realistic workload without depending on another repository during CI.

## Release Checks

Before promoting a release, the documentation and source distribution should
survive the same checks as the code:

```bash
docker build -t vmod-wasm-ci .
docker run --rm vmod-wasm-ci make check
docker run --rm vmod-wasm-ci make distcheck DISTCHECK_CONFIGURE_FLAGS="--with-wasmtime=/opt/wasmtime"
```

Use `make perf-test` for a short local throughput comparison and
`make soak-test` for longer VCL reload, pooling, and sustained-traffic checks.
