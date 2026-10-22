# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

THIS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

INSTALL_DIR ?= $(THIS_DIR)install

LIGHTTPD_SRC ?= $(THIS_DIR)lighttpd-1.4.59
LIGHTTPD_HASH ?= e266e389ddb79bf17b8e8d9022aec95ae839c6f3159822f402df8d8df8a13f65

LIGHTTPD_MIRRORS ?= \
    https://download.lighttpd.net/lighttpd/releases-1.4.x \
    https://packages.gramineproject.io/distfiles

HOST ?= 127.0.0.1
PORT ?= 8003

ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
else
SAGUARO_LOG_LEVEL = error
endif

CONF_FILES = lighttpd-server.conf lighttpd.conf

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs, dev docs: vbs-enclave/manual.md): lighttpd built with the vbs
# gcc (no OpenSSL, PCRE, zlib or bzip2: not in the vbs sysroot) and installed with its modules, its
# configuration and the test files into vbs-build/root/lighttpd, packed by vbs.manifest.template
VBS_APP = lighttpd
VBS_ROOT = vbs-build/root/lighttpd
VBS_RANDOM_FILES = $(foreach n,1 2 3 4 5 6 7 8 9 10,2K.$n.html) $(foreach n,1 2 3 4 5,10K.$n.html) \
	$(foreach n,1 2 3 4 5,100K.$n.html) $(foreach n,1 2 3,1M.$n.html) $(foreach n,1 2 3,100.$n.html)
VBS_TEST_DATA = $(addprefix $(VBS_ROOT)/html/random/,$(VBS_RANDOM_FILES))
VBS_DEPS = $(VBS_ROOT)/sbin/lighttpd $(VBS_ROOT)/conf/lighttpd-vbs.conf $(VBS_TEST_DATA)
VBS_CLEAN = vbs-build
include ../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all: $(INSTALL_DIR)/sbin/lighttpd lighttpd.manifest $(CONF_FILES) testdata
ifeq ($(SGX),1)
all: lighttpd.manifest.sgx lighttpd.sig
endif
endif

$(INSTALL_DIR)/sbin/lighttpd: $(LIGHTTPD_SRC)/configure
	cd $(LIGHTTPD_SRC) && ./configure --prefix=$(abspath $(INSTALL_DIR)) \
		--without-openssl --without-pcre --without-bzip2
	cd $(LIGHTTPD_SRC) && $(MAKE)
	cd $(LIGHTTPD_SRC) && $(MAKE) install

$(LIGHTTPD_SRC)/configure: $(LIGHTTPD_SRC).tar.gz
	tar -xzf $<
	# Refresh the timestamp, but only for this file - otherwise ./configure starts recreating too
	# much.
	touch $(LIGHTTPD_SRC)/configure

$(LIGHTTPD_SRC).tar.gz:
	../common_tools/download --output $@ --sha256 $(LIGHTTPD_HASH) \
		$(foreach mirror,$(LIGHTTPD_MIRRORS),--url $(mirror)/$(LIGHTTPD_SRC).tar.gz)

ifeq ($(VBS),1)
# gcc 15 defaults to C23, in which lighttpd (and its build helper lemon.c, old-style definitions)
# does not compile: the C17 dialect of earlier gcc versions for both
VBS_LIGHTTPD_CFLAGS := $(shell [ "$$(gcc -dumpversion | cut -d. -f1)" -ge 15 ] 2>/dev/null && echo '-g -O2 -std=gnu17')
$(VBS_ROOT)/sbin/lighttpd: $(LIGHTTPD_SRC).tar.gz | $(VBS_RUNTIME_STAMP)
	rm -rf vbs-build/src && mkdir -p vbs-build/src
	tar -C vbs-build/src --strip-components=1 -xzf $<
	touch vbs-build/src/configure
	cd vbs-build/src && ./configure CC=$(VBS_GCC) $(if $(VBS_LIGHTTPD_CFLAGS),CFLAGS="$(VBS_LIGHTTPD_CFLAGS)" CFLAGS_FOR_BUILD="$(VBS_LIGHTTPD_CFLAGS)") --prefix=/lighttpd \
		--without-openssl --without-pcre --without-bzip2 --without-zlib > ../configure.log
	$(MAKE) -C vbs-build/src
	$(MAKE) -C vbs-build/src install DESTDIR=$(abspath vbs-build/root)

$(VBS_ROOT)/conf/lighttpd-vbs.conf: lighttpd-vbs.conf.template $(VBS_ROOT)/sbin/lighttpd
	mkdir -p $(dir $@)
	sed -e 's|$$(PORT)|$(PORT)|g' -e 's|$$(HOST)|$(HOST)|g' $< > $@

$(VBS_ROOT)/html/random/%.html: $(VBS_ROOT)/sbin/lighttpd
	mkdir -p $(VBS_ROOT)/html/random
	dd if=/dev/urandom of=$@ count=1 bs=$(basename $(basename $(notdir $@))) status=none

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" PORT=$(PORT) ./vbs-check.sh
endif

lighttpd.manifest: lighttpd.manifest.template
	saguaro-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		-Dinstall_dir=$(abspath $(INSTALL_DIR)) \
		$< >$@

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# see the helloworld example for details on this workaround.
lighttpd.manifest.sgx lighttpd.sig: sgx_sign
	@:

.INTERMEDIATE: sgx_sign
sgx_sign: lighttpd.manifest $(INSTALL_DIR)/sbin/lighttpd
	saguaro-sgx-sign \
		--manifest $< \
		--output $<.sgx

# lighttpd configuration and test data
lighttpd-server.conf:
	@$(RM) $@
	@echo "server.document-root       = \"$(abspath $(INSTALL_DIR))/html\""    >> $@
	@echo "server.port                = $(PORT)"            >> $@
	@echo "server.bind                = \"$(HOST)\""        >> $@

lighttpd.conf:
	@$(RM) $@
	@echo "include \"./lighttpd-server.conf\""              >> $@
	@echo "include \"./lighttpd-generic.conf\""             >> $@

# Generate variously-sized HTML files in $(RANDOM_DIR)
RANDOM_DIR = $(INSTALL_DIR)/html/random
RANDOM_FILES = \
	$(foreach n,1 2 3 4 5 6 7 8 9 10,2K.$n.html) \
	$(foreach n,1 2 3 4 5,10K.$n.html) \
	$(foreach n,1 2 3 4 5,100K.$n.html) \
	$(foreach n,1 2 3,1M.$n.html) \
	$(foreach n,1 2 3,10M.$n.html) \
	$(foreach n,1 2 3,100.$n.html)

TEST_DATA = $(addprefix $(RANDOM_DIR)/,$(RANDOM_FILES))

$(RANDOM_DIR)/%.html:
	mkdir -p $(RANDOM_DIR)
	dd if=/dev/urandom of=$@ count=1 bs=$(basename $(basename $(notdir $@))) status=none

.PHONY: testdata
testdata: $(TEST_DATA)

.PHONY: start-native-server
start-native-server: all
	$(INSTALL_DIR)/sbin/lighttpd -D -m $(INSTALL_DIR)/lib -f lighttpd.conf

ifeq ($(VBS),1)
# GRAMINE is saguaro-vbs (Makefile.vbs)
else ifeq ($(SGX),)
GRAMINE = saguaro-direct
else
GRAMINE = saguaro-sgx
endif

# Note that command-line arguments are hardcoded in the manifest file.
.PHONY: start-saguaro-server
start-saguaro-server: all
	$(GRAMINE) ./lighttpd

ifneq ($(VBS),1)
# the server under $(GRAMINE), a client on the host's loopback (check.sh)
.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./check.sh
endif

.PHONY: clean
clean:
	$(RM) \
		*.manifest *.manifest.sgx *.token *.sig OUTPUT result-* $(CONF_FILES)

.PHONY: distclean
distclean: clean
	$(RM) -r $(LIGHTTPD_SRC).tar.gz $(LIGHTTPD_SRC) $(INSTALL_DIR) $(TEST_DATA) *.pem
