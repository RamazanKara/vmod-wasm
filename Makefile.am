ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src

CARGO ?= cargo
DOCKER ?= docker
DOCKER_IMAGE ?= vmod-wasm-ci
WASMTIME_DIR ?= /opt/wasmtime
WASM_TARGET_DIR = $(abs_top_builddir)/examples-target
WASM_FIXTURE_DIR = $(abs_top_builddir)/tests/wasm
WASM_RELEASE_DIR = $(WASM_TARGET_DIR)/wasm32-unknown-unknown/release

EXTRA_DIST = \
	.editorconfig \
	.dockerignore \
	.gitattributes \
	.gitignore \
	README.md \
	LICENSE \
	SECURITY.md \
	CODE_OF_CONDUCT.md \
	THIRD_PARTY_NOTICES.md \
	CONTRIBUTING.md \
	CHANGELOG.md \
	Dockerfile \
	autogen.sh \
	scripts/perf-test.sh \
	scripts/soak-test.sh \
	.github/dependabot.yml \
	.github/PULL_REQUEST_TEMPLATE.md \
	.github/ISSUE_TEMPLATE/bug_report.md \
	.github/ISSUE_TEMPLATE/config.yml \
	.github/ISSUE_TEMPLATE/feature_request.md \
	.github/workflows/ci.yml \
	.github/workflows/release.yml \
	docs/ARCHITECTURE.md \
	docs/COMPATIBILITY.md \
	docs/CONFIGURATION.md \
	docs/DEVELOPMENT.md \
	docs/PRODUCTION.md \
	docs/SECURITY.md \
	examples/Cargo.lock \
	examples/Cargo.toml \
	examples/README.md \
	examples/rustfmt.toml \
	examples/rust/Cargo.toml \
	examples/rust/src/lib.rs \
	examples/proxy-wasm-filter/Cargo.toml \
	examples/proxy-wasm-filter/src/lib.rs \
	examples/passthrough/Cargo.toml \
	examples/passthrough/src/lib.rs \
	examples/transform/Cargo.toml \
	examples/transform/src/lib.rs \
	examples/edge-security-filter/Cargo.toml \
	examples/edge-security-filter/README.md \
	examples/edge-security-filter/config.json \
	examples/edge-security-filter/src/config.rs \
	examples/edge-security-filter/src/lib.rs \
	tests/allow_decision.vtc \
	tests/basic_load.vtc \
	tests/block_decision.vtc \
	tests/error_handling.vtc \
	tests/filter_chain.vtc \
	tests/fuel_exhaustion.vtc \
	tests/host_block_bot.vtc \
	tests/host_header.vtc \
	tests/host_url_method.vtc \
	tests/memory_limit.vtc \
	tests/pool_stats.vtc \
	tests/proxy_wasm_block.vtc \
	tests/proxy_wasm_edge_filter_basic.vtc \
	tests/proxy_wasm_edge_filter_bot.vtc \
	tests/proxy_wasm_edge_filter_callout.vtc \
	tests/proxy_wasm_edge_filter_config.vtc \
	tests/proxy_wasm_edge_filter_metrics.vtc \
	tests/proxy_wasm_edge_filter_ratelimit.vtc \
	tests/proxy_wasm_lifecycle.vtc \
	tests/proxy_wasm_metrics.vtc \
	tests/proxy_wasm_response.vtc \
	tests/proxy_wasm_response_body.vtc \
	tests/proxy_wasm_sdk.vtc \
	tests/proxy_wasm_security.vtc \
	tests/resource_config.vtc \
	tests/vcl_reload_lifecycle.vtc \
	tests/wasm/test_module.wasm

# Unit tests cannot link standalone against the VMOD because Varnish internal
# symbols (http_SetH, VRT_priv_task_get, etc.) are only available inside
# varnishd at runtime. Integration testing is done via VTC tests below.

.PHONY: build build-wasm-test-fixtures lint fmt audit test docker-test perf-test soak-test release-dry-run

build: all build-wasm-test-fixtures

build-wasm-test-fixtures:
	@if ! command -v "$(CARGO)" >/dev/null 2>&1; then \
		echo "ERROR: cargo not found; install Rust or use Docker"; \
		exit 1; \
	fi
	@mkdir -p "$(WASM_FIXTURE_DIR)"
	@cd "$(abs_top_srcdir)/examples" && \
		CARGO_TARGET_DIR="$(WASM_TARGET_DIR)" \
		"$(CARGO)" build --release --target wasm32-unknown-unknown
	@cp "$(WASM_RELEASE_DIR)/test_module.wasm" "$(WASM_FIXTURE_DIR)/"
	@cp "$(WASM_RELEASE_DIR)/proxy_wasm_filter.wasm" "$(WASM_FIXTURE_DIR)/"
	@cp "$(WASM_RELEASE_DIR)/passthrough.wasm" "$(WASM_FIXTURE_DIR)/"
	@cp "$(WASM_RELEASE_DIR)/transform.wasm" "$(WASM_FIXTURE_DIR)/"
	@cp "$(WASM_RELEASE_DIR)/edge_security_filter.wasm" "$(WASM_FIXTURE_DIR)/"

lint:
	cd "$(abs_top_srcdir)/examples" && "$(CARGO)" fmt --all --check
	cd "$(abs_top_srcdir)/examples" && "$(CARGO)" clippy --target wasm32-unknown-unknown --all-targets -- -D warnings

fmt:
	cd "$(abs_top_srcdir)/examples" && "$(CARGO)" fmt --all

audit:
	@if ! "$(CARGO)" audit --version >/dev/null 2>&1; then \
		"$(CARGO)" install cargo-audit --locked; \
	fi
	cd "$(abs_top_srcdir)/examples" && "$(CARGO)" audit

test: docker-test

docker-test:
	$(DOCKER) build -t "$(DOCKER_IMAGE)" "$(abs_top_srcdir)"
	$(DOCKER) run --rm "$(DOCKER_IMAGE)" make check

perf-test:
	"$(abs_top_srcdir)/scripts/perf-test.sh" --image "$(DOCKER_IMAGE)"

soak-test:
	"$(abs_top_srcdir)/scripts/soak-test.sh" --image "$(DOCKER_IMAGE)"

release-dry-run:
	$(DOCKER) build -t "$(DOCKER_IMAGE)" "$(abs_top_srcdir)"
	$(DOCKER) run --rm "$(DOCKER_IMAGE)" make check
	$(DOCKER) run --rm "$(DOCKER_IMAGE)" make distcheck DISTCHECK_CONFIGURE_FLAGS="--with-wasmtime=$(WASMTIME_DIR)"

# Run VTC tests (integration tests inside varnishd)
check-local: build-wasm-test-fixtures
	@if test -n "$(VARNISHTEST)"; then \
		for t in $(abs_top_srcdir)/tests/*.vtc; do \
			echo "Running $$t..."; \
			$(VARNISHTEST) -t 120 \
				-Dvmod_wasm="$(abs_top_builddir)/src/.libs/libvmod_wasm.so" \
				-Dwasm_module="$(WASM_FIXTURE_DIR)/test_module.wasm" \
				-Dwasm_passthrough="$(WASM_FIXTURE_DIR)/passthrough.wasm" \
				-Dwasm_transform="$(WASM_FIXTURE_DIR)/transform.wasm" \
				-Dsdk_module="$(WASM_FIXTURE_DIR)/proxy_wasm_filter.wasm" \
				-Dedge_filter="$(WASM_FIXTURE_DIR)/edge_security_filter.wasm" \
				$$t || exit 1; \
		done; \
	else \
		echo "WARNING: varnishtest not found, skipping VTC tests"; \
	fi

clean-local:
	rm -rf "$(WASM_TARGET_DIR)" "$(WASM_FIXTURE_DIR)"
