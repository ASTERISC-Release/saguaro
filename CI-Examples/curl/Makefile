# Copyright (C) 2024 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

THIS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
CURL_DIR ?= /usr/bin

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

CURL_VERSION = 8.22.0
CURL_SHA256 = d54dd598bf05927a726deb38df31c6a255ba83ff1de57c5d1464dac3ed8f44a1

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs, Documentation/vbs.rst): curl built from the source
# tarball with the vbs gcc (against the vbs glibc), trimmed to HTTP on the loopback (no TLS, no
# DNS threads, no protocols but HTTP), in vbs-build/, packed by vbs.manifest.template
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

ifeq ($(VBS),1)
curl-$(CURL_VERSION).tar.gz:
	../common_tools/download --output $@ --sha256 $(CURL_SHA256) \
		--url https://curl.se/download/curl-$(CURL_VERSION).tar.gz

# The trimmed build: no TLS library (the vbs sysroot has none), no compression, no IDN, no LDAP, no
# SSH, no threaded resolver (the enclave creates threads only on request); the programs of the
# configure tests run on the Linux side
vbs-build/curl: curl-$(CURL_VERSION).tar.gz | $(VBS_RUNTIME_STAMP)
	rm -rf vbs-build/src && mkdir -p vbs-build/src
	tar -C vbs-build/src --strip-components=1 -xzf $<
	cd vbs-build/src && CC=$(VBS_GCC) ./configure \
		--disable-shared --without-ssl --without-zlib --without-brotli --without-zstd \
		--without-libpsl --without-libidn2 --without-nghttp2 --without-libssh --without-librtmp \
		--without-gssapi --without-ca-bundle --without-ca-path --disable-ldap --disable-ldaps \
		--disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 \
		--disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-smb \
		--disable-ftp --disable-file --disable-threaded-resolver --disable-docs --disable-manual \
		--disable-netrc --disable-unix-sockets --disable-alt-svc --disable-hsts \
		--disable-dependency-tracking --disable-ipv6 --disable-progress-meter \
		--disable-versioned-symbols --disable-libcurl-option \
		CFLAGS="-O2"
	$(MAKE) -C vbs-build/src
	cp vbs-build/src/src/curl $@

.PHONY: check
check: all
	(cd test-docroot; exec python3 -m http.server -b 127.0.0.1 19111) & httpd_pid=$$!; \
	../common_tools/wait_for_server 5 127.0.0.1 19111; \
	GRAMINE="$(GRAMINE)" ./vbs-check.sh; rc=$$?; \
	kill $$httpd_pid; exit $$rc
else
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

ifeq ($(SGX),)
GRAMINE = gramine-direct
else
GRAMINE = gramine-sgx
endif

.PHONY: check
check: all
	(cd test-docroot; exec python3 -m http.server -b 127.0.0.1 19111) & httpd_pid=$$!; \
	../common_tools/wait_for_server 5 127.0.0.1 19111; \
	$(GRAMINE) ./curl http://127.0.0.1:19111/ > OUTPUT; rc=$$?; \
	kill $$httpd_pid; exit $$rc

	@grep -q "Hello World" OUTPUT && echo "[ Success 1/1 ]"
	@rm OUTPUT
endif

.PHONY: clean
clean:
	$(RM) *.manifest *.manifest.sgx *.token *.sig OUTPUT
