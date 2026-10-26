# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

# glxgears under saguaro-vm with the Tirith graphics redirection (x86_64). See
# dev docs: graphics/ for the host setup (graphics QEMU of vmm/graphics, X proxy).

# The graphics preload library is built and installed with Saguaro (-Dgraphics=enabled and a
# VM-type PAL); the manifest refers to it as saguaro.libgraphics.
GRAPHICS_LIB = $(shell python3 -c "import saguarolibos; print(saguarolibos._CONFIG_PKGLIBDIR)")/libgraphics.so

CFLAGS = -Wall -Wextra
LDLIBS = -lGL -lX11 -lm

# Input goes through the shared-memory ring in the comm page; 0 falls back to the X connection.
SHARED_INPUT ?= 1


ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
CFLAGS += -g
else
SAGUARO_LOG_LEVEL = error
CFLAGS += -O3
endif

.PHONY: all
all: glxgears glxgears.manifest

glxgears: glxgears.o
	$(CC) $^ -o $@ $(LDLIBS)

glxgears.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: FORCE
glxgears.manifest: glxgears.manifest.template FORCE
	saguaro-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Dshared_input=$(SHARED_INPUT) \
		$< $@
	saguaro-manifest-check $@

# Without Saguaro: the preload library presents through its own X connection.
.PHONY: run-native
run-native: all
	LD_PRELOAD=$(GRAPHICS_LIB) EGL_DISCRETE=$${EGL_DISCRETE:-0} ./glxgears

.PHONY: run
run: all
	mkdir -p $$HOME/.cache
	saguaro-vm glxgears

# saguaro-direct on the host GPU (dev docs: graphics-direct/): stock Mesa with DRI3 by
# default; PRELOAD=1 uses the preload library instead (needs an install built with a VM-type PAL).
# The X server is the one of $(DISPLAY), reached through its Unix socket.
PRELOAD ?= 0
# The host's library directory of this architecture, and the NVIDIA driver's device nodes in the
# direct manifest when the host has them (NVIDIA=0 or 1 overrides).
ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)
NVIDIA ?= $(if $(wildcard /dev/nvidiactl),1,0)
X_DISPLAY_NUM = $(patsubst :%,%,$(firstword $(subst ., ,$(DISPLAY))))

glxgears-direct.manifest: glxgears-direct.manifest.template FORCE
	saguaro-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Ddisplay=$(X_DISPLAY_NUM) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		-Dnvidia=$(NVIDIA) \
		-Dpreload=$(PRELOAD) \
		-Degl_discrete=$${EGL_DISCRETE:-0} \
		$< $@
	saguaro-manifest-check $@

.PHONY: run-direct
run-direct: glxgears glxgears-direct.manifest
	saguaro-direct glxgears-direct $(GLXGEARS_ARGS)

.PHONY: clean
clean:
	$(RM) *.manifest glxgears.o glxgears

.PHONY: distclean
distclean: clean
