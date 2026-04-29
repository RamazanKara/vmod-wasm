ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src

EXTRA_DIST = \
	README.md \
	LICENSE \
	autogen.sh

# Run VTC tests
check-local:
	@if test -n "$(VARNISHTEST)"; then \
		for t in $(srcdir)/tests/*.vtc; do \
			echo "Running $$t..."; \
			$(VARNISHTEST) \
				-Dvmod_wasm="$(abs_top_builddir)/src/.libs/libvmod_wasm.so" \
				-Dwasm_module="$(abs_top_srcdir)/tests/wasm/test_module.wasm" \
				$$t || exit 1; \
		done; \
	else \
		echo "WARNING: varnishtest not found, skipping VTC tests"; \
	fi
