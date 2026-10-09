# SPDX-License-Identifier: BSD-3-Clause
ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)
CFLAGS = -O2 -Wall -Wextra

.PHONY: all
all: syscall_bench syscall_bench.manifest

syscall_bench: syscall_bench.c

syscall_bench.manifest: syscall_bench.manifest.template
	gramine-manifest -Dlog_level=error -Darch_libdir=$(ARCH_LIBDIR) $< $@

.PHONY: bench
bench: all
	@echo "== native =="; ./syscall_bench $(ITERS) $(REPS)
	@echo "== gramine-direct =="; gramine-direct syscall_bench $(ITERS) $(REPS)

.PHONY: clean
clean:
	$(RM) syscall_bench syscall_bench.manifest
