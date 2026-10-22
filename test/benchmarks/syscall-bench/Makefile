# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# REWRITE=1|0 forces sys.rewrite_syscalls on/off in the manifest; empty = LibOS default
REWRITE ?=
ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)
CFLAGS = -O2 -Wall -Wextra

.PHONY: all
all: syscall_bench syscall_bench.manifest

syscall_bench: syscall_bench.c

.PHONY: syscall_bench.manifest
syscall_bench.manifest: syscall_bench.manifest.template
	saguaro-manifest -Dlog_level=error -Darch_libdir=$(ARCH_LIBDIR) -Drewrite=$(REWRITE) $< $@

.PHONY: bench
bench: all
	@echo "== native =="; ./syscall_bench $(ITERS) $(REPS)
	@echo "== saguaro-direct =="; saguaro-direct syscall_bench $(ITERS) $(REPS)

.PHONY: clean
clean:
	$(RM) syscall_bench syscall_bench.manifest
