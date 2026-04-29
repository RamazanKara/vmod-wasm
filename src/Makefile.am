AM_CFLAGS = $(VARNISHAPI_CFLAGS) $(WASMTIME_CFLAGS) -I$(top_builddir)/src

vmoddir = $(VMOD_DIR)
vmod_LTLIBRARIES = libvmod_wasm.la

libvmod_wasm_la_SOURCES = \
	vmod_wasm.c \
	wasm_engine.c \
	wasm_engine.h

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

vcc_if.c vcc_if.h: vmod_wasm.vcc
	$(VMODTOOL) $(srcdir)/vmod_wasm.vcc

BUILT_SOURCES = vcc_if.c vcc_if.h
CLEANFILES = $(BUILT_SOURCES)
EXTRA_DIST = vmod_wasm.vcc
