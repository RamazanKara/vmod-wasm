AM_CFLAGS = $(VARNISHAPI_CFLAGS) $(WASMTIME_CFLAGS) -I$(top_builddir)/src

vmoddir = $(VMOD_DIR)
vmod_LTLIBRARIES = libvmod_wasm.la

libvmod_wasm_la_SOURCES = \
	vmod_wasm.c \
	wasm_engine.c \
	wasm_engine.h \
	vdp_wasm.c \
	vdp_wasm.h \
	host_functions.c \
	host_functions.h \
	wasi_types.h \
	proxy_wasm.c \
	proxy_wasm.h \
	proxy_wasm_mem.h \
	proxy_wasm_headers.c \
	proxy_wasm_properties.c \
	proxy_wasm_http.c \
	proxy_wasm_shared.c \
	proxy_wasm_shared.h \
	store_pool.c \
	store_pool.h \
	http_pool.c \
	http_pool.h \
	compat.h

libvmod_wasm_la_LDFLAGS = \
	-module \
	-export-dynamic \
	-avoid-version \
	-shared

libvmod_wasm_la_LIBADD = $(VARNISHAPI_LIBS) $(WASMTIME_LIBS)

# Generate VMOD interface from .vcc
nodist_libvmod_wasm_la_SOURCES = \
	vcc_if.c \
	vcc_if.h

vmod_vcs_version.txt:
	echo "$(PACKAGE_VERSION)" > $@

vcc_if.c vcc_if.h: vmod_wasm.vcc vmod_vcs_version.txt
	$(VMODTOOL) $(srcdir)/vmod_wasm.vcc

BUILT_SOURCES = vcc_if.c vcc_if.h
CLEANFILES = \
	$(BUILT_SOURCES) \
	vmod_vcs_version.txt \
	vmod_wasm.man.rst \
	vmod_wasm.rst
EXTRA_DIST = vmod_wasm.vcc
