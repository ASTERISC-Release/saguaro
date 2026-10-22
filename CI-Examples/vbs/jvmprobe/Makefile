# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Research probe (dev docs: vbs-enclave/PLAN-rwx.md): Temurin 21 (OpenJDK) on an x64 VBS
# enclave, to see which memory requests the JVM makes that a VBS enclave refuses. Not an example that is
# expected to work. x86_64 only (the tarball is the x64 JRE).
#   make VBS=1; saguaro-vbs jvmprobe

JRE_VERSION = 21.0.12.1_1
JRE_TARBALL = OpenJDK21U-jre_x64_linux_hotspot_$(JRE_VERSION).tar.gz
JRE_SHA256 = 2413149700df0f7d440500a84a8f764c535f21e5a5e87d38328b64eec2c5b500
JRE_URL = https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/$(JRE_TARBALL)

.PHONY: all
ifeq ($(VBS),1)
VBS_APP = jvmprobe
VBS_DEPS = vbs-build/jre/release vbs-build/lib/libstdc++.so.6 vbs-build/lib/libgcc_s.so.1
VBS_CLEAN = vbs-build
include ../../../scripts/vbs/Makefile.vbs
all: vbs-all

$(JRE_TARBALL):
	../../common_tools/download --output $@ --sha256 $(JRE_SHA256) --url $(JRE_URL)

vbs-build/jre/release: $(JRE_TARBALL)
	mkdir -p vbs-build/jre
	tar -C vbs-build/jre --strip-components=1 -xzf $(JRE_TARBALL)
	touch $@
vbs-build/lib/libstdc++.so.6 vbs-build/lib/libgcc_s.so.1:
	mkdir -p vbs-build/lib
	cp -L "$$($(CC) -print-file-name=$(notdir $@))" $@
else
all:
	@echo "this research probe is for the VBS backend only: make VBS=1"
endif

.PHONY: check
check: all
	$(GRAMINE) jvmprobe

.PHONY: clean
clean:
	$(RM) -r jvmprobe.vbs jvmprobe.manifest.vbs OUTPUT
