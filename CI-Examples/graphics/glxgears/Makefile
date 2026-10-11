# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

# glxgears under gramine-vm with the Tirith graphics redirection (x86_64). See
# Documentation/devel/graphics/ for the host setup (QEMU fork, X proxy).

# The graphics preload library is built and installed with Gramine (-Dgraphics=enabled and a
# VM-type PAL); the manifest refers to it as gramine.libgraphics.
GRAPHICS_LIB = $(shell python3 -c "import graminelibos; print(graminelibos._CONFIG_PKGLIBDIR)")/libgraphics.so

CFLAGS = -Wall -Wextra
LDLIBS = -lGL -lX11 -lm

# Input goes through the shared-memory ring in the comm page; 0 falls back to the X connection.
SHARED_INPUT ?= 1

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
CFLAGS += -g
else
GRAMINE_LOG_LEVEL = error
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
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Dshared_input=$(SHARED_INPUT) \
		$< $@
	gramine-manifest-check $@

# Without Gramine: the preload library presents through its own X connection.
.PHONY: run-native
run-native: all
	LD_PRELOAD=$(GRAPHICS_LIB) EGL_DISCRETE=$${EGL_DISCRETE:-0} ./glxgears

.PHONY: run
run: all
	mkdir -p $$HOME/.cache
	gramine-vm glxgears

.PHONY: clean
clean:
	$(RM) *.manifest glxgears.o glxgears

.PHONY: distclean
distclean: clean
