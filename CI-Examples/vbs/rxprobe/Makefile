# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Research probe (PLAN-rwx.md, "Runtime executable memory: options"); not in any default build. `make VBS=1`, `saguaro-vbs rxprobe`.

.PHONY: all
ifeq ($(VBS),1)
VBS_APP = rxprobe
VBS_TEMPLATE = rxprobe.manifest
VBS_DEPS = rxprobe.c
VBS_CLEAN = rxprobe
include ../../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all:
	@echo "this example is for the VBS backend only: make VBS=1"
endif

.PHONY: check
check: all
	$(GRAMINE) rxprobe

.PHONY: clean
clean:
	$(RM) -r rxprobe rxprobe.vbs rxprobe.manifest.vbs OUTPUT
