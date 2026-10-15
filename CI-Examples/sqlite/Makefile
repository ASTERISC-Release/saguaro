# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

SQLITE_AMALGAMATION = sqlite-amalgamation-3450100
SQLITE_AMALGAMATION_SHA256 = 5592243caf28b2cdef41e6ab58d25d653dfc53deded8450eb66072c929f030c4

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs, Documentation/vbs.rst): the shell built from the
# amalgamation with the vbs gcc (static PIE, vbs glibc) in vbs-build/, packed by vbs.manifest.template
VBS_APP = sqlite3
VBS_DEPS = vbs-build/sqlite3 scripts/create.sql scripts/update.sql scripts/select.sql
VBS_CLEAN = vbs-build OUTPUT
include ../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all: sqlite3.manifest
ifeq ($(SGX),1)
all: sqlite3.manifest.sgx sqlite3.sig
endif
endif

sqlite3.manifest: manifest.template
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Dexecdir=$(shell dirname $(shell which sqlite3)) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		$< >$@

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# see the helloworld example for details on this workaround.
sqlite3.manifest.sgx sqlite3.sig: sgx_sign
	@:

.INTERMEDIATE: sgx_sign
sgx_sign: sqlite3.manifest
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

# Note that command-line arguments are hardcoded in the manifest file.
.PHONY: regression
regression: all
	@$(RM) db/*

	$(GRAMINE) sqlite3 < scripts/create.sql
	@echo "[ Success 1/3 ]"

	$(GRAMINE) sqlite3 < scripts/update.sql
	@echo "[ Success 2/3 ]"

	$(GRAMINE) sqlite3 < scripts/select.sql > OUTPUT
	diff OUTPUT scripts/select.txt
	@echo "[ Success 3/3 ]"

ifeq ($(VBS),1)
sqlite-amalgamation.zip:
	../common_tools/download --output $@ --sha256 $(SQLITE_AMALGAMATION_SHA256) \
		--url https://www.sqlite.org/2024/$(SQLITE_AMALGAMATION).zip

vbs-build/src/sqlite3.c: sqlite-amalgamation.zip
	rm -rf vbs-build/src && mkdir -p vbs-build/src
	unzip -q -j -d vbs-build/src sqlite-amalgamation.zip

# no readline, no editline, no dlopen (no extensions) and one thread: the enclave has no terminal
# input and no libraries outside the vbs sysroot (zlib is not used by the shell's core commands)
vbs-build/sqlite3: vbs-build/src/sqlite3.c | $(VBS_RUNTIME_STAMP)
	$(VBS_CC_STATIC) -O2 -DSQLITE_THREADSAFE=0 -DHAVE_READLINE=0 -DHAVE_EDITLINE=0 \
		-DSQLITE_OMIT_LOAD_EXTENSION -o $@ vbs-build/src/shell.c vbs-build/src/sqlite3.c

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./vbs-check.sh
endif

.PHONY: clean
clean:
	$(RM) *.manifest *.manifest.sgx *.token *.sig OUTPUT db/*

.PHONY: distclean
distclean: clean
