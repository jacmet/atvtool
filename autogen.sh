#!/bin/sh
autoreconf -v --install || exit 1
CFLAGS=-Wall ./configure --enable-maintainer-mode "$@"
