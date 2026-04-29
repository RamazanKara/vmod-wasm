AC_PREREQ([2.69])
AC_INIT([vmod-wasm], [0.1.0], [https://github.com/RamazanKara/vmod-wasm/issues])
AC_CONFIG_SRCDIR([src/vmod_wasm.vcc])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([1.16 foreign -Wall -Werror])
AM_SILENT_RULES([yes])
AM_PROG_AR

LT_PREREQ([2.4])
LT_INIT([dlopen disable-static])

AC_PROG_CC
AC_PROG_CC_C99
AC_PROG_INSTALL

# Check for pkg-config
PKG_PROG_PKG_CONFIG

# Find Varnish
PKG_CHECK_MODULES([VARNISHAPI], [varnishapi >= 8.0])

# Find vmodtool.py
AC_ARG_VAR([VMODTOOL], [path to vmodtool.py])
if test -z "$VMODTOOL"; then
    VMODTOOL="$($PKG_CONFIG --variable=vmodtool varnishapi)"
fi
if test -z "$VMODTOOL" || ! test -f "$VMODTOOL"; then
    AC_MSG_ERROR([cannot find vmodtool.py — install varnish-dev])
fi

# Varnish directories
VMOD_DIR="$($PKG_CONFIG --variable=vmoddir varnishapi)"
AC_SUBST([VMOD_DIR])

VARNISH_VMOD_INCLUDE="$($PKG_CONFIG --variable=pkgincludedir varnishapi)"
AC_SUBST([VARNISH_VMOD_INCLUDE])

# Find varnishtest
AC_ARG_VAR([VARNISHTEST], [path to varnishtest])
if test -z "$VARNISHTEST"; then
    AC_PATH_PROG([VARNISHTEST], [varnishtest])
fi

# Find Wasmtime C API
AC_ARG_WITH([wasmtime],
    [AS_HELP_STRING([--with-wasmtime=DIR], [path to Wasmtime C API installation])],
    [WASMTIME_DIR="$withval"],
    [WASMTIME_DIR=""])

if test -n "$WASMTIME_DIR"; then
    WASMTIME_CFLAGS="-I${WASMTIME_DIR}/include"
    WASMTIME_LIBS="-L${WASMTIME_DIR}/lib -lwasmtime"
else
    # Try pkg-config first, then default paths
    PKG_CHECK_MODULES([WASMTIME], [wasmtime], [], [
        # Fall back to checking standard paths
        AC_CHECK_HEADER([wasmtime.h], [],
            [AC_MSG_ERROR([cannot find wasmtime.h — install Wasmtime C API or use --with-wasmtime=DIR])])
        WASMTIME_LIBS="-lwasmtime"
    ])
fi

AC_SUBST([WASMTIME_CFLAGS])
AC_SUBST([WASMTIME_LIBS])

# Platform-specific linker flags for Wasmtime (Rust static lib)
case "$host_os" in
    linux*)
        WASMTIME_LIBS="$WASMTIME_LIBS -lpthread -ldl -lm"
        ;;
    darwin*)
        WASMTIME_LIBS="$WASMTIME_LIBS -lpthread"
        ;;
esac

AC_CONFIG_FILES([
    Makefile
    src/Makefile
])

AC_OUTPUT
