#!/bin/sh
# Bootstrap the autotools build system
set -e

mkdir -p m4
libtoolize --force --copy
aclocal -I m4
autoheader
automake --add-missing --foreign
autoconf

echo ""
echo "Now run: ./configure [--with-wasmtime=/path/to/wasmtime]"
echo ""
