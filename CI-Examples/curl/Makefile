# Copyright (C) 2024 Gramine contributors
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause

# Build curl as follows:
#
# - make               -- create non-SGX manifest (system curl)
# - make SGX=1         -- create SGX manifest
# - make VBS=1         -- build curl for a Windows VBS enclave (from source, against the vbs glibc,
#                         HTTP only), pack and sign it (in WSL; see dev docs: vbs-enclave/manual.md);
#                         make VBS=1 check runs it against a server in WSL

THIS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
CURL_DIR ?= /usr/bin

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

# Address of the test server in `make check`. In a vm or sev guest 127.0.0.1 is the guest itself, so
# those need an address of the host that is not loopback (exec.sh passes it).
HTTP_ADDR ?= 127.0.0.1

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs): curl built from the release tarball with the vbs
# gcc, static libcurl, no TLS and no optional protocols (vbs-build/curl, a dynamic PIE)
VBS_APP = curl
VBS_DEPS = vbs-build/curl
VBS_CLEAN = vbs-build OUTPUT
include ../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all: curl.manifest
ifeq ($(SGX),1)
all: curl.manifest.sgx curl.sig
endif
endif

curl.manifest: curl.manifest.template
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Dhome=$(HOME) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		-Dcurl_dir=$(CURL_DIR) \
		$< >$@

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# for details on this workaround see
# https://github.com/gramineproject/gramine/blob/e8735ea06c/CI-Examples/helloworld/Makefile
curl.manifest.sgx curl.sig: sgx_sign
	@:

.INTERMEDIATE: sgx_sign
sgx_sign: curl.manifest
	gramine-sgx-sign \
		--manifest $< \
		--output $<.sgx

ifeq ($(VBS),1)
# GRAMINE is gramine-vbs (Makefile.vbs)
else ifeq ($(SGX),)
GRAMINE = gramine-direct
else
GRAMINE = gramine-sgx
endif

ifeq ($(VBS),1)
CURL_VERSION = 8.5.0
CURL_VBS_SHA256 = 42ab8db9e20d8290a3b633e7fbb3cec15db34df65fd1015ef8ac1e4723750eeb

curl-$(CURL_VERSION).tar.xz:
	../common_tools/download --output $@ \
		--sha256 $(CURL_VBS_SHA256) \
		--url https://curl.se/download/curl-$(CURL_VERSION).tar.xz \
		--url https://github.com/curl/curl/releases/download/curl-$(subst .,_,$(CURL_VERSION))/curl-$(CURL_VERSION).tar.xz

vbs-build/src/configure: curl-$(CURL_VERSION).tar.xz
	mkdir -p vbs-build/src
	tar -C vbs-build/src --strip-components=1 -xf $<
	touch $@

# HTTP only: no TLS library, no zlib, no threaded resolver (one thread), static libcurl. The
# protocols that need other libraries or processes are left out.
vbs-build/src/Makefile: vbs-build/src/configure | $(VBS_RUNTIME_STAMP)
	cd vbs-build/src && ./configure CC=$(VBS_GCC) --disable-shared --enable-static \
		--without-ssl --without-zlib --without-brotli --without-zstd --without-libpsl \
		--without-libidn2 --without-nghttp2 --without-libssh --without-librtmp \
		--without-gssapi --without-ngtcp2 --without-nghttp3 --without-quiche \
		--disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet \
		--disable-tftp --disable-pop3 --disable-imap --disable-smtp --disable-gopher \
		--disable-mqtt --disable-smb --disable-ftp --disable-file --disable-threaded-resolver \
		--disable-docs --disable-manual --disable-libcurl-option
	touch $@

vbs-build/curl: vbs-build/src/Makefile
	$(MAKE) -C vbs-build/src/lib CC=$(VBS_GCC)
	$(MAKE) -C vbs-build/src/src CC=$(VBS_GCC)
	cp vbs-build/src/src/curl $@

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./vbs-check.sh
else
.PHONY: check
check: all
	(cd test-docroot; exec python3 -m http.server -b $(HTTP_ADDR) 19111) & httpd_pid=$$!; \
	../common_tools/wait_for_server 5 $(HTTP_ADDR) 19111; \
	$(GRAMINE) ./curl http://$(HTTP_ADDR):19111/ > OUTPUT; rc=$$?; \
	kill $$httpd_pid; exit $$rc

	@grep -q "Hello World" OUTPUT && echo "[ Success 1/1 ]"
	@rm OUTPUT
endif

.PHONY: clean
clean:
	$(RM) *.manifest *.manifest.sgx *.token *.sig OUTPUT

.PHONY: distclean
distclean: clean
