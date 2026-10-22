# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

SRCDIR = src
MEMCACHED_SRC ?= memcached-1.6.21.tar.gz
MEMCACHED_MIRRORS ?= \
    https://memcached.org/files \
    https://packages.gramineproject.io/distfiles

MEMCACHED_SHA256 ?= c788980efc417dd5d93c442b1c8b8769fb2018896c29de3887d22a2f143da2ee

ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
else
SAGUARO_LOG_LEVEL = error
endif

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs, dev docs: vbs-enclave/manual.md): libevent and memcached
# built with the vbs gcc (libevent linked statically: it is not in the vbs sysroot) in vbs-build/,
# packed by vbs.manifest.template
VBS_APP = memcached
VBS_DEPS = vbs-build/memcached
VBS_CLEAN = vbs-build OUTPUT
LIBEVENT_VERSION = 2.1.12-stable
LIBEVENT_SHA256 = 92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb
include ../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all: memcached memcached.manifest
ifeq ($(SGX),1)
all: memcached.manifest.sgx memcached.sig
endif
endif

$(SRCDIR)/configure:
	../common_tools/download --output memcached.tar.gz --sha256 $(MEMCACHED_SHA256) \
		$(foreach mirror,$(MEMCACHED_MIRRORS),--url $(mirror)/$(MEMCACHED_SRC))
	mkdir $(SRCDIR)
	tar -C $(SRCDIR) --strip-components=1 -xf memcached.tar.gz

$(SRCDIR)/memcached: $(SRCDIR)/configure
	cd $(SRCDIR) && ./configure
	$(MAKE) -C $(SRCDIR)

ifeq ($(VBS),1)
libevent-$(LIBEVENT_VERSION).tar.gz:
	../common_tools/download --output $@ --sha256 $(LIBEVENT_SHA256) \
		--url https://github.com/libevent/libevent/releases/download/release-$(LIBEVENT_VERSION)/libevent-$(LIBEVENT_VERSION).tar.gz

vbs-build/libevent/lib/libevent.a: libevent-$(LIBEVENT_VERSION).tar.gz | $(VBS_RUNTIME_STAMP)
	rm -rf vbs-build/libevent-src && mkdir -p vbs-build/libevent-src
	tar -C vbs-build/libevent-src --strip-components=1 -xzf $<
	cd vbs-build/libevent-src && ./configure CC=$(VBS_GCC) --prefix=$(abspath vbs-build/libevent) \
		--disable-shared --disable-openssl --disable-samples --disable-libevent-regress \
		--with-pic > ../libevent-configure.log
	$(MAKE) -C vbs-build/libevent-src install > vbs-build/libevent-make.log

vbs-build/memcached: vbs-build/libevent/lib/libevent.a | $(VBS_RUNTIME_STAMP)
	../common_tools/download --output memcached.tar.gz --sha256 $(MEMCACHED_SHA256) \
		$(foreach mirror,$(MEMCACHED_MIRRORS),--url $(mirror)/$(MEMCACHED_SRC))
	rm -rf vbs-build/src && mkdir -p vbs-build/src
	tar -C vbs-build/src --strip-components=1 -xf memcached.tar.gz
	cd vbs-build/src && ./configure CC=$(VBS_GCC) --with-libevent=$(abspath vbs-build/libevent) \
		--disable-sasl --disable-extstore --disable-docs > ../configure.log
	$(MAKE) -C vbs-build/src
	cp vbs-build/src/memcached $@

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./vbs-check.sh
endif

memcached.manifest: memcached.manifest.template
	gramine-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		$< > $@

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# see the helloworld example for details on this workaround.
memcached.manifest.sgx memcached.sig: sgx_sign
	@:

.INTERMEDIATE: sgx_sign
sgx_sign: memcached.manifest memcached
	gramine-sgx-sign \
		--manifest $< \
		--output $<.sgx

# for simplicity, copy memcached executable into our root directory
memcached: $(SRCDIR)/memcached
	cp $< $@

.PHONY: start-native-server
start-native-server: all
	./memcached

ifeq ($(VBS),1)
# GRAMINE is gramine-vbs (Makefile.vbs)
else ifeq ($(SGX),)
GRAMINE = gramine-direct
else
GRAMINE = gramine-sgx
endif

.PHONY: start-gramine-server
start-gramine-server: all
	$(GRAMINE) memcached

ifneq ($(VBS),1)
# the server under $(GRAMINE), a client on the host's loopback (check.sh)
.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./check.sh
endif

.PHONY: clean
clean:
	$(RM) *.token *.sig *.manifest.sgx *.manifest memcached .lck

.PHONY: distclean
distclean: clean
	$(RM) -r $(SRCDIR) memcached.tar.gz
