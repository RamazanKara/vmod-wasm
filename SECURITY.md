# Security Policy

## Supported Versions

Security fixes are provided for the latest stable release line.

| Version | Supported |
|---------|-----------|
| 4.3.x | Yes |
| < 4.3 | No |

The public release support target is Varnish 9.x on Linux amd64 and arm64.
Stable release tags use the `varnish9-vX.Y.Z` format.

## Reporting A Vulnerability

Please report suspected vulnerabilities privately by opening a GitHub security
advisory for this repository. If advisories are unavailable, email the
maintainer listed on the GitHub profile and include `vmod-wasm security` in the
subject.

Include:

- Affected vmod-wasm version or commit SHA.
- Varnish, Wasmtime, and operating system versions.
- Reproduction steps or proof of concept.
- Expected impact and whether the issue is exploitable remotely.

Please do not publish exploit details until a fix or mitigation is available.

## Security Model

The detailed runtime threat model, isolation boundaries, and production
hardening guidance live in [docs/SECURITY.md](docs/SECURITY.md).
