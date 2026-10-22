#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# nginx CI example on hosts without distro libssl-dev/libpcre-dev:
#  - PCRE: built standalone into deps/pcre-install; nginx gets the bare --with-pcre
#    (which is what sets USE_PCRE=YES and compiles src/core/ngx_regex.c into nginx;
#    the argument form --with-pcre=DIR only builds the static lib, see nginx 1.22
#    auto/options + auto/modules:663) plus cc/ld options pointing at our install.
#  - OpenSSL: nginx's own --with-openssl=DIR source build (works as is).
# Otherwise mirrors CI-Examples/nginx/Makefile: same core flags, same three
# objs/ngx_auto_config.h seds, make install, final `make` for manifest + certs.
set -e
THIS_DIR="$(cd "$(dirname "$0")" && pwd)"
NGINX_VERSION=1.22.0
OPENSSL_VERSION=1.1.1w
PCRE_VERSION=8.45
# Needs the Gramine tools on PATH (saguaro-manifest for the final `make`); otherwise pick them up
# from SAGUARO_PREFIX (default ~/.local/saguaro) like the other scripts do.
if ! command -v saguaro-manifest >/dev/null; then
  . "$THIS_DIR/../../scripts/saguaro-env.sh"
fi
cd "$THIS_DIR"
NGINX_SRC="$THIS_DIR/nginx-$NGINX_VERSION"
DEPS="$THIS_DIR/deps"
mkdir -p "$DEPS"

# The Makefile rule is keyed on the relative path (./nginx-VERSION/configure); an absolute path has no rule.
make "nginx-$NGINX_VERSION/configure"

[ -x "$DEPS/pcre-install/lib/libpcre.a" ] || {
  [ -d "$DEPS/pcre-$PCRE_VERSION" ] || {
    curl -fsSL -o "$DEPS/pcre-$PCRE_VERSION.tar.gz" \
      "https://sourceforge.net/projects/pcre/files/pcre/$PCRE_VERSION/pcre-$PCRE_VERSION.tar.gz/download"
    tar --touch -xzf "$DEPS/pcre-$PCRE_VERSION.tar.gz" -C "$DEPS"
  }
  [ -f "$DEPS/pcre-$PCRE_VERSION/Makefile" ] || \
    (cd "$DEPS/pcre-$PCRE_VERSION" && ./configure --prefix="$DEPS/pcre-install" \
      --enable-static --disable-shared --with-pic >/dev/null)
  make -C "$DEPS/pcre-$PCRE_VERSION" -j"$(nproc)" >/dev/null
  make -C "$DEPS/pcre-$PCRE_VERSION" install >/dev/null
}

[ -d "$DEPS/openssl-$OPENSSL_VERSION" ] || {
  curl -fsSL -o "$DEPS/openssl-$OPENSSL_VERSION.tar.gz" \
    "https://www.openssl.org/source/old/${OPENSSL_VERSION%[a-z]}/openssl-$OPENSSL_VERSION.tar.gz"
  tar --touch -xzf "$DEPS/openssl-$OPENSSL_VERSION.tar.gz" -C "$DEPS"
}

cd "$NGINX_SRC"
# Key "already configured" on objs/Makefile: a failed ./configure leaves objs/ngx_auto_config.h
# behind without a Makefile, which would otherwise be mistaken for a finished configure.
if [ ! -f objs/Makefile ]; then
  rm -rf objs
  ./configure --prefix="$THIS_DIR/install" \
    --without-http_rewrite_module --with-http_ssl_module \
    --with-pcre \
    --with-cc-opt="-I$DEPS/pcre-install/include" \
    --with-ld-opt="-L$DEPS/pcre-install/lib -lpcre" \
    --with-openssl="$DEPS/openssl-$OPENSSL_VERSION"
  grep -q ngx_regex objs/Makefile || { echo "FATAL: regex module still not in objs/Makefile"; exit 1; }
fi
sed -e "s|#define NGX_HAVE_EVENTFD[[:space:]]\+1|#define NGX_HAVE_EVENTFD 0|g" \
    -e "s|#define NGX_HAVE_SYS_EVENTFD_H[[:space:]]\+1|#define NGX_HAVE_SYS_EVENTFD_H 0|g" \
    -e "s|#define NGX_HAVE_PR_SET_DUMPABLE[[:space:]]\+1|#define NGX_HAVE_PR_SET_DUMPABLE 0|g" \
    -i objs/ngx_auto_config.h
make -j"$(nproc)"
make install

cd "$THIS_DIR"
make
echo "nginx built; now run:  saguaro-direct nginx  (serves http on 8002, https on 8444)"
