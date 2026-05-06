ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src

EXTRA_DIST = \
	README.md \
	LICENSE \
	autogen.sh

# Unit tests cannot link standalone against the VMOD because Varnish internal
# symbols (http_SetH, VRT_priv_task_get, etc.) are only available inside
# varnishd at runtime. Integration testing is done via VTC tests below.

# Run VTC tests (integration tests inside varnishd)
check-local:
	@if test -n "$(VARNISHTEST)"; then \
		for t in $(srcdir)/tests/*.vtc; do \
			echo "Running $$t..."; \
			$(VARNISHTEST) -t 120 \
				-Dvmod_wasm="$(abs_top_builddir)/src/.libs/libvmod_wasm.so" \
				-Dwasm_module="$(abs_top_srcdir)/tests/wasm/test_module.wasm" \
				-Dwasm_passthrough="$(abs_top_srcdir)/tests/wasm/passthrough.wasm" \
				-Dwasm_transform="$(abs_top_srcdir)/tests/wasm/transform.wasm" \
				-Dsdk_module="$(abs_top_srcdir)/tests/wasm/proxy_wasm_filter.wasm" \
				$$t || exit 1; \
		done; \
	else \
		echo "WARNING: varnishtest not found, skipping VTC tests"; \
	fi
