# Contributing to vmod-wasm

Thank you for considering a contribution to vmod-wasm.

## Prerequisites

- Rust toolchain (stable) with the `wasm32-unknown-unknown` target
- Docker (for running the full test suite)
- Varnish 9.x development headers (for native builds)
- Wasmtime C API 49.0.2, autoconf, automake, libtool, make, pkg-config, a C
  compiler and Python 3 (for native builds)
- curl and tar (for pinned companion-filter test sources)

Install the Rust target:

```shell
rustup target add wasm32-unknown-unknown
rustup component add clippy rustfmt
```

## Development Workflow

For native development, generate and configure the build first:

```shell
./autogen.sh
./configure --with-wasmtime=/path/to/wasmtime
```

1. **Build the VMOD and Wasm test fixtures:**

   ```shell
   make build
   ```

2. **Run lints:**

   ```shell
   make lint
   ```

3. **Run dependency audit checks:**

   ```shell
   make audit
   ```

4. **Run the local gate:**

   ```shell
   make lint check build
   ```

   `make check` runs C shared-store unit tests, Rust configuration tests, and VTC
   integration tests. A missing `varnishtest` is an error. The fixture build
   downloads companion repositories at pinned revisions and uses Cargo locks.

   For Docker-backed tests:

   ```shell
   make test
   ```

5. **Format code:**

   ```shell
   make fmt
   ```

6. **Run a soak test for lifecycle, pooling, reload, or concurrency changes:**

   ```shell
   make soak-test
   ```

7. **Run the full release dry-run before release-impacting changes:**

   ```shell
   make release-dry-run
   ```

## Writing a New Wasm Module

1. Create a new crate under `examples/`:

   ```shell
   cargo init --lib examples/my-module
   ```

2. Add it to the workspace in `examples/Cargo.toml`:

   ```toml
   members = [
       # ... existing members
       "my-module",
   ]
   ```

3. Set the crate type to `cdylib` in your module's `Cargo.toml`:

   ```toml
   [lib]
   crate-type = ["cdylib"]
   ```

4. Implement the Proxy-Wasm ABI (use the `proxy-wasm` SDK) or the raw vmod-wasm host functions.

5. Write a `.vtc` integration test under `tests/` — see existing tests for examples.

## Code Style

- `make lint` checks Rust formatting and treats clippy warnings as errors.
- CI runs the same `make lint check build` gate in Docker on pushes and manual
  dispatch. Run it locally when GitHub Actions is unavailable.

## Commit Messages

Use conventional commits:

```
feat(module): add rate limiting to edge-security-filter
fix(transform): handle empty response headers
docs: update ARCHITECTURE.md with new module
test: add VTC for bot detection edge case
```

## Pull Request Process

1. Create a feature branch from `main`.
2. Ensure the local gate passes (`make lint check build`, or the Docker command
   in the README).
3. Update relevant documentation if behavior changes.
4. Request review from a maintainer.

## Release Tagging

Stable release tags include the supported Varnish ABI line:

The release workflow is triggered by a pushed `varnish9-vX.Y.Z` tag.

The package version remains semantic (`X.Y.Z`); the tag prefix makes the
Varnish support line explicit for release assets.

## Reporting Issues

Open a GitHub issue with:

- Varnish version
- Wasmtime C API version (from the installed runtime; the Docker pin is in `Dockerfile`)
- Release tag or commit SHA
- Steps to reproduce
- Expected vs actual behavior
