# Copyright (C) 2023 Gramine contributors
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause

# Build SQLite as follows:
#
# - make               -- create non-SGX manifest
# - make SGX=1         -- create SGX manifest
# - make VBS=1         -- build the SQLite shell for a Windows VBS enclave, pack and sign it (in
#                         WSL; see Documentation/vbs.md); make VBS=1 check runs the three scripts
#                         in the enclave and compares the output

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs): the shell built from the amalgamation with the vbs
# gcc (static PIE against the vbs glibc, no readline, no zlib, no threads), packed by
# vbs.manifest.template together with the scripts
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
ifneq ($(filter gramine-vm gramine-sev,$(GRAMINE)),)
	# The VM and SEV launchers print console lines to stdout; drop them before the diff. The
	# patterns are those of vmfilter() in test/vm-multiproc/vm-examples.sh, plus the line the PAL
	# added later ('Application address space').
	grep -v -e '^Configuring Gramine VM' -e '^REM_MEM_SIZE' -e '^\[boot\]' -e '^(PAL) Physical' \
		-e '^Private frame pool' -e 'self-check passed' -e '^\[ VM ' -e '^vm-relay:' \
		-e '^Application address space' \
		OUTPUT > OUTPUT.filtered; mv OUTPUT.filtered OUTPUT
endif
	diff OUTPUT scripts/select.txt
	@echo "[ Success 3/3 ]"

ifeq ($(VBS),1)
SQLITE_AMALGAMATION = sqlite-amalgamation-3450100
SQLITE_AMALGAMATION_SHA256 = 5592243caf28b2cdef41e6ab58d25d653dfc53deded8450eb66072c929f030c4

sqlite-amalgamation.zip:
	../common_tools/download --output $@ \
		--sha256 $(SQLITE_AMALGAMATION_SHA256) \
		--url https://www.sqlite.org/2024/$(SQLITE_AMALGAMATION).zip

# Without readline and zlib (not in the vbs sysroot), single-threaded, no extension loading. The
# shell's .system and .shell commands use system() and popen() (the scripts do not use them).
vbs-build/sqlite3: sqlite-amalgamation.zip | $(VBS_RUNTIME_STAMP)
	rm -rf vbs-build/amalgamation
	mkdir -p vbs-build/amalgamation
	unzip -q -j sqlite-amalgamation.zip -d vbs-build/amalgamation
	$(VBS_CC_STATIC) -O2 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DHAVE_READLINE=0 \
		-o $@ vbs-build/amalgamation/shell.c vbs-build/amalgamation/sqlite3.c

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./vbs-check.sh
endif

.PHONY: clean
clean:
	$(RM) *.manifest *.manifest.sgx *.token *.sig OUTPUT db/*

.PHONY: distclean
distclean: clean
