ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src

EXTRA_DIST = \
	README.md \
	LICENSE \
	autogen.sh

# Unit test programs
check_PROGRAMS = \
	tests/test_store_pool \
	tests/test_http_pool \
	tests/test_trailer_map

tests_test_store_pool_SOURCES = tests/test_store_pool.c
tests_test_store_pool_CFLAGS = $(WASMTIME_CFLAGS) -I$(top_srcdir)/src
tests_test_store_pool_LDADD = src/libvmod_wasm.la $(WASMTIME_LIBS) -lpthread

tests_test_http_pool_SOURCES = tests/test_http_pool.c
tests_test_http_pool_CFLAGS = $(WASMTIME_CFLAGS) -I$(top_srcdir)/src
tests_test_http_pool_LDADD = src/libvmod_wasm.la $(WASMTIME_LIBS) -lpthread

tests_test_trailer_map_SOURCES = tests/test_trailer_map.c
tests_test_trailer_map_CFLAGS = $(WASMTIME_CFLAGS) -I$(top_srcdir)/src
tests_test_trailer_map_LDADD = src/libvmod_wasm.la $(WASMTIME_LIBS) -lpthread

TESTS = $(check_PROGRAMS)

# Run VTC tests
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
