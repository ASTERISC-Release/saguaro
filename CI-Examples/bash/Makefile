# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
else
SAGUARO_LOG_LEVEL = error
endif

.PHONY: all
all: bash.manifest
ifeq ($(SGX),1)
all: bash.manifest.sgx bash.sig
endif

bash.manifest: manifest.template
	saguaro-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Dexecdir=$(shell dirname $(shell which bash)) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		$< >$@

bash.manifest.sgx: bash.manifest
	saguaro-sgx-sign \
		--manifest bash.manifest \
		--output $@

bash.sig: bash.manifest.sgx

ifeq ($(SGX),)
SAGUARO = saguaro-direct
else
SAGUARO = saguaro-sgx
endif

.PHONY: regression
regression: all
	@mkdir -p scripts/testdir

	$(SAGUARO) ./bash -c "ls" > OUTPUT
	@grep -q "Makefile" OUTPUT && echo "[ Success 1/7 ]"
	@rm OUTPUT

	$(SAGUARO) ./bash -c "cd scripts && bash bash_test.sh 1" > OUTPUT
	@grep -q "hello 1" OUTPUT      && echo "[ Success 2/7 ]"
	@grep -q "createdfile" OUTPUT  && echo "[ Success 3/7 ]"
	@grep -q "somefile" OUTPUT     && echo "[ Success 4/7 ]"
	@grep -q "current date" OUTPUT && echo "[ Success 5/7 ]"
	@rm OUTPUT

	$(SAGUARO) ./bash -c "cd scripts && bash bash_test.sh 3" > OUTPUT
	@grep -q "hello 3" OUTPUT      && echo "[ Success 6/7 ]"
	@rm OUTPUT

	$(SAGUARO) ./bash -c "readlink /proc/self/exe" > OUTPUT
	@grep -qE "^(/usr)?/bin/readlink" OUTPUT && echo "[ Success 7/7 ]"
	@rm OUTPUT

	@rm -rf scripts/testdir


.PHONY: clean
clean:
	$(RM) *.manifest *.manifest.sgx *.token *.sig OUTPUT scripts/testdir/*

.PHONY: distclean
distclean: clean
