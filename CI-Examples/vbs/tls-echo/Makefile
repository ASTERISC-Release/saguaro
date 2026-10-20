# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause
#
# TLS echo server inside a Windows VBS enclave, its certificate bound to the enclave's attestation
# report (README.md). Only for VBS: `make` here means `make VBS=1`.
#   make                  build mbedtls and the server for the enclave, render, pack, link and sign
#   make check            run the server and the Windows client (vbs-tls-client) with its negative tests
#   make VBS_DEBUG=1      the same image as a debuggable enclave (tls-echo-debug.vbs; for the
#                         client's "debug image" test)
#   make clean

VBS = 1
VBS_DEBUG ?= 0
ifeq ($(VBS_DEBUG),1)
VBS_APP = tls-echo-debug
VBS_DEFINES = -Ddebug=true
else
VBS_APP = tls-echo
VBS_DEFINES = -Ddebug=false
endif
VBS_TEMPLATE = vbs.manifest.template
VBS_DEPS = vbs-build/tls-echo
VBS_CLEAN = vbs-build OUTPUT* tls-echo-debug.vbs tls-echo-debug.manifest.vbs
include ../../../scripts/vbs/Makefile.vbs

.PHONY: all check
all: vbs-all

# mbedtls 3.6.0, the version the LibOS bundles (subprojects/mbedtls-3.6.0.wrap), built with the vbs gcc
# (x18 reserved, against the vbs glibc) as static libraries for the application; its default
# configuration (TLS 1.2 and 1.3, X.509 writing, entropy from getrandom()); mbedtls takes the AES and
# PMULL instructions when AT_HWCAP advertises them, which the LibOS passes on AArch64
# (dev docs: aarch64/hwcap/: the same throughput as forcing them with
# MBEDTLS_AES_USE_HARDWARE_ONLY, about 3 times the software fallback's)
MBEDTLS_VERSION = 3.6.0
MBEDTLS_SHA256 = 3ecf94fcfdaacafb757786a01b7538a61750ebd85c4b024f56ff8ba1490fcd38
MBEDTLS_CACHE = $(GRAMINE_VBS_REPO)/subprojects/packagecache/mbedtls-$(MBEDTLS_VERSION).tar.bz2
MBEDTLS_DIR = vbs-build/mbedtls
MBEDTLS_LIBS = $(MBEDTLS_DIR)/library/libmbedtls.a $(MBEDTLS_DIR)/library/libmbedx509.a \
	$(MBEDTLS_DIR)/library/libmbedcrypto.a

vbs-build/mbedtls.tar.bz2:
	mkdir -p vbs-build
	if [ -f $(MBEDTLS_CACHE) ]; then cp $(MBEDTLS_CACHE) $@; else \
		../../common_tools/download --output $@ --sha256 $(MBEDTLS_SHA256) \
			--url https://github.com/Mbed-TLS/mbedtls/releases/download/v$(MBEDTLS_VERSION)/mbedtls-$(MBEDTLS_VERSION).tar.bz2 \
			--url https://packages.gramineproject.io/distfiles/mbedtls-$(MBEDTLS_VERSION).tar.bz2; fi
	echo "$(MBEDTLS_SHA256)  $@" | sha256sum -c

$(MBEDTLS_DIR)/library/libmbedtls.a: vbs-build/mbedtls.tar.bz2 Makefile | $(VBS_RUNTIME_STAMP)
	rm -rf $(MBEDTLS_DIR)
	mkdir -p $(MBEDTLS_DIR)
	tar -C $(MBEDTLS_DIR) --strip-components=1 -xjf $<
	$(MAKE) -C $(MBEDTLS_DIR)/library CC=$(VBS_GCC) CFLAGS="-O2" \
		libmbedcrypto.a libmbedx509.a libmbedtls.a

vbs-build/tls-echo: tls-echo.c ../common/vbs-ratls.c ../common/vbs-ratls.h $(MBEDTLS_DIR)/library/libmbedtls.a
	$(VBS_GCC) -O2 -Wall -Wextra -I$(MBEDTLS_DIR)/include -I../common -o $@ $< ../common/vbs-ratls.c \
		$(MBEDTLS_LIBS)

# the check also builds the debuggable variant (for the "debug enclave" test)
check: all
	$(MAKE) VBS_DEBUG=1 all
	GRAMINE="$(GRAMINE)" ./vbs-check.sh
