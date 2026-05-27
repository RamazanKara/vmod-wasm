# Contributing to vmod-wasm

Thank you for considering a contribution to vmod-wasm.

## Prerequisites

- Rust toolchain (stable) with the `wasm32-unknown-unknown` target
- Docker (for running the full test suite)
- Varnish 9.x development headers (for native builds)

Install the Rust target:

```shell
rustup target add wasm32-unknown-unknown
```

## Development Workflow

For native development, generate and configure the build first:

```shell
./autogen.sh
./configure --with-wasmtime=/path/to/wasmtime
```

1. **Build Wasm modules:**

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

4. **Run the full test suite (Docker):**

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

- Run `cargo fmt` before committing (enforced in CI).
- All clippy warnings are treated as errors in CI.
- Keep `.wasm` binaries under 500 KiB (release build).

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
2. Ensure all CI checks pass (`make lint && make test`).
3. Update relevant documentation if behavior changes.
4. Request review from a maintainer.

## Release Tagging

Stable release tags include the supported Varnish ABI line:

```shell
git tag varnish9-vX.Y.Z
git push origin varnish9-vX.Y.Z
```

The package version remains semantic (`X.Y.Z`); the tag prefix makes the
Varnish support line explicit for release assets.

## Reporting Issues

Open a GitHub issue with:

- Varnish version
- Wasmtime version (from `configure.ac`)
- Release tag or commit SHA
- Steps to reproduce
- Expected vs actual behavior
