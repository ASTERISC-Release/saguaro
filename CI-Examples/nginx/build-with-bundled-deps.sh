#!/bin/bash
# Build the nginx CI example without distro libssl-dev/libpcre-dev (neither is required
# by nginx itself: its configure can build both from source trees). Use this when the
# example's plain `make` fails at ./configure with "SSL modules require the OpenSSL
# library" or "the HTTP rewrite module requires the PCRE library" and the -dev packages
# cannot be installed.
#
# Apart from the added --with-openssl=<src> and --with-pcre=<src> options, this mirrors
# CI-Examples/nginx/Makefile exactly: same configure flags, the same three
# objs/ngx_auto_config.h seds (Gramine-incompatible feature macros), then make install
# and a final `make` to generate the manifest and the self-signed SSL fixtures.
#
# Example: aarch64 hosts without those packages (the Tirith aarch64 port box).
set -e
THIS_DIR="$(cd "$(dirname "$0")" && pwd)"
NGINX_VERSION=1.22.0            # keep in sync with NGINX_SRC in the Makefile
OPENSSL_VERSION=1.1.1w          # nginx 1.22 supports the 1.1.1 (and 3.0) series
PCRE_VERSION=8.45
export PATH=/home/user/.local/gramine/bin:$PATH
export PYTHONPATH=/home/user/.local/gramine/lib/python3.12/site-packages
cd "$THIS_DIR"
NGINX_SRC="$THIS_DIR/nginx-$NGINX_VERSION"
DEPS="$THIS_DIR/deps"
mkdir -p "$DEPS"

# nginx sources via the Makefile's own hash-pinned download rule
make "$NGINX_SRC/configure"

[ -d "$DEPS/pcre-$PCRE_VERSION" ] || {
  curl -fsSL -o "$DEPS/pcre-$PCRE_VERSION.tar.gz" \
    "https://sourceforge.net/projects/pcre/files/pcre/$PCRE_VERSION/pcre-$PCRE_VERSION.tar.gz/download"
  tar --touch -xzf "$DEPS/pcre-$PCRE_VERSION.tar.gz" -C "$DEPS"
}
[ -d "$DEPS/openssl-$OPENSSL_VERSION" ] || {
  curl -fsSL -o "$DEPS/openssl-$OPENSSL_VERSION.tar.gz" \
    "https://www.openssl.org/source/old/${OPENSSL_VERSION%[a-z]}/openssl-$OPENSSL_VERSION.tar.gz"
  tar --touch -xzf "$DEPS/openssl-$OPENSSL_VERSION.tar.gz" -C "$DEPS"
}

cd "$NGINX_SRC"
if [ ! -f objs/ngx_auto_config.h ]; then
  ./configure --prefix="$THIS_DIR/install" \
    --without-http_rewrite_module --with-http_ssl_module \
    --with-openssl="$DEPS/openssl-$OPENSSL_VERSION" \
    --with-pcre="$DEPS/pcre-$PCRE_VERSION"
fi
sed -e "s|#define NGX_HAVE_EVENTFD[[:space:]]\+1|#define NGX_HAVE_EVENTFD 0|g" \
    -e "s|#define NGX_HAVE_SYS_EVENTFD_H[[:space:]]\+1|#define NGX_HAVE_SYS_EVENTFD_H 0|g" \
    -e "s|#define NGX_HAVE_PR_SET_DUMPABLE[[:space:]]\+1|#define NGX_HAVE_PR_SET_DUMPABLE 0|g" \
    -i objs/ngx_auto_config.h
make -j"$(nproc)"
make install

cd "$THIS_DIR"
make
echo "nginx built; now run:  gramine-direct nginx  (serves http on 8002, https on 8444)"
