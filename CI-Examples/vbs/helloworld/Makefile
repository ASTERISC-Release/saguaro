# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# `make VBS=1` builds and packages this example for a Windows VBS enclave (dev docs: vbs-enclave/manual.md,
# "Makefile flow"); `saguaro-vbs helloworld` runs it and `make VBS=1 check` checks the output.

.PHONY: all
ifeq ($(VBS),1)
VBS_APP = helloworld
VBS_TEMPLATE = helloworld.manifest
VBS_DEPS = helloworld.c
VBS_CLEAN = helloworld
include ../../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all:
	@echo "this example is for the VBS backend only: make VBS=1"
endif

.PHONY: check
check: all
	$(SAGUARO) helloworld > OUTPUT
	grep -q "^Hello, world from a VBS enclave (pid [0-9]*, 0 arguments)$$" OUTPUT
	@echo "[ Success ]"

.PHONY: clean
clean:
	$(RM) -r helloworld helloworld.vbs helloworld.manifest.vbs OUTPUT
