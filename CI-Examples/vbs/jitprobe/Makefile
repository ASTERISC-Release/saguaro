# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# `make VBS=1` builds and packages this example for a Windows VBS enclave (dev docs: vbs-enclave/manual.md,
# "Makefile flow"); `saguaro-vbs jitprobe` runs it and `make VBS=1 check` prints what the enclave answers (a report, no verdict).

.PHONY: all
ifeq ($(VBS),1)
VBS_APP = jitprobe
VBS_TEMPLATE = jitprobe.manifest
VBS_DEPS = jitprobe.c
VBS_CLEAN = jitprobe
include ../../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all:
	@echo "this example is for the VBS backend only: make VBS=1"
endif

.PHONY: check
check: all
	$(GRAMINE) jitprobe

.PHONY: clean
clean:
	$(RM) -r jitprobe jitprobe.vbs jitprobe.manifest.vbs OUTPUT
