---
name: Bug report
about: Report a reproducible vmod-wasm problem
labels: bug
---

## Environment

- vmod-wasm version:
- Release tag or commit SHA:
- Varnish version:
- Wasmtime version:
- OS/architecture:

## Reproduction

Steps, VCL, Wasm module, or VTC test case that reproduces the issue.

Include the relevant `vcl_init` configuration and whether the module uses
Proxy-Wasm HTTP callouts, response body filtering, or VCL reloads.

## Expected Behavior

What should happen?

## Actual Behavior

What happened instead? Include logs where useful.
