# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

# Test programs for graphics under gramine-direct (x86_64), see
# dev docs: graphics-direct/. `make check` runs each natively and under gramine-direct.

PROGRAMS = drm_info gbm_map egl_readback ioctl_bench x_roundtrip gl_bench egl_surfaceless x_input
NODE ?= /dev/dri/renderD128
# The host's library directory of this architecture, and the NVIDIA driver's device nodes when the
# host has them (NVIDIA=0 or 1 overrides).
ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)
NVIDIA ?= $(if $(wildcard /dev/nvidiactl),1,0)
# the X server of $(DISPLAY) (display 0 when DISPLAY is not set)
X_DISPLAY_NUM = $(or $(patsubst :%,%,$(firstword $(subst ., ,$(DISPLAY)))),0)

CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags libdrm)
LDLIBS_drm_info = $(shell pkg-config --libs libdrm)
LDLIBS_gbm_map = $(shell pkg-config --libs gbm)
LDLIBS_egl_readback = $(shell pkg-config --libs gbm egl glesv2)

ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
else
SAGUARO_LOG_LEVEL = error
endif

.PHONY: all
all: $(PROGRAMS) $(addsuffix .manifest,$(PROGRAMS))

drm_info: drm_info.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS_drm_info)

gbm_map: gbm_map.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS_gbm_map)

ioctl_bench: ioctl_bench.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS_drm_info)

gl_bench: gl_bench.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags gbm egl glesv2) $< -o $@ $(LDLIBS_egl_readback)

# X key and button events: ./x_input [seconds] (send keys with xdotool key --window <id> ...)
x_input: x_input.c
	$(CC) $(CFLAGS) $< -o $@ $(shell pkg-config --libs x11)

x_roundtrip: x_roundtrip.c
	$(CC) $(CFLAGS) $< -o $@ $(shell pkg-config --libs xcb)

# EGL surfaceless platform, no GBM (works with the NVIDIA driver too); not part of `check`, it
# takes no device argument: ./egl_surfaceless [frames] [draws]
egl_surfaceless: egl_surfaceless.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags egl glesv2) $< -o $@ $(shell pkg-config --libs egl glesv2)

egl_readback: egl_readback.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags gbm egl glesv2) $< -o $@ $(LDLIBS_egl_readback)

%.manifest: direct-tests.manifest.template FORCE
	gramine-manifest -Dlog_level=$(SAGUARO_LOG_LEVEL) -Darch_libdir=$(ARCH_LIBDIR) -Dnvidia=$(NVIDIA) -Ddisplay=$(X_DISPLAY_NUM) -Dentrypoint=$* $< $@
	gramine-manifest-check $@

.PHONY: FORCE

.PHONY: check
check: all
	@for p in $(filter-out ioctl_bench x_roundtrip gl_bench egl_surfaceless x_input,$(PROGRAMS)); do \
		echo "== $$p native"; ./$$p $(NODE) || exit 1; \
		echo "== $$p gramine-direct"; gramine-direct $$p $(NODE) || exit 1; \
	done

.PHONY: eglinfo
eglinfo: eglinfo.manifest
	gramine-direct eglinfo -B

eglinfo.manifest: direct-tests.manifest.template FORCE
	gramine-manifest -Dlog_level=$(SAGUARO_LOG_LEVEL) -Darch_libdir=$(ARCH_LIBDIR) -Dnvidia=$(NVIDIA) -Ddisplay=$(X_DISPLAY_NUM) -Dentrypoint=/usr/bin/eglinfo $< $@
	gramine-manifest-check $@

.PHONY: clean
clean:
	$(RM) $(PROGRAMS) *.manifest
