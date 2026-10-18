# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# `make VBS=1` builds and packages this example for a Windows VBS enclave (Documentation/vbs.md,
# "Makefile flow"); `gramine-vbs python` runs it and `make VBS=1 check` checks the output.

.PHONY: all
ifeq ($(VBS),1)
VBS_APP = python
VBS_TEMPLATE = vbs.manifest.template
VBS_DEPS = hello.py
VBS_CLEAN = python
include ../../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all:
	@echo "this example is for the VBS backend only: make VBS=1"
endif

.PHONY: check
check: all
	$(GRAMINE) python > OUTPUT
	grep -q "Python 3.12.7" OUTPUT
	@echo "[ Success ]"

.PHONY: clean
clean:
	$(RM) -r python python.vbs python.manifest.vbs OUTPUT
