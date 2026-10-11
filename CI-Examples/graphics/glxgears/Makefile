# SPDX-License-Identifier: BSD-3-Clause

# glxgears under gramine-vm with the Tirith graphics redirection (x86_64). See
# Documentation/devel/graphics/ for the host setup (QEMU fork, X proxy).

EGL_BACKEND_DIR = $(abspath ../egl-backend)

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
all: egl_backend glxgears glxgears.manifest

.PHONY: egl_backend
egl_backend:
	$(MAKE) -C $(EGL_BACKEND_DIR)

glxgears: glxgears.o
	$(CC) $^ -o $@ $(LDLIBS)

glxgears.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: FORCE
glxgears.manifest: glxgears.manifest.template FORCE
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Degl_backend=$(EGL_BACKEND_DIR)/egl_backend.so \
		-Dshared_input=$(SHARED_INPUT) \
		$< $@
	gramine-manifest-check $@

# Without Gramine: the preload library presents through its own X connection.
.PHONY: run-native
run-native: all
	LD_PRELOAD=$(EGL_BACKEND_DIR)/egl_backend.so EGL_DISCRETE=$${EGL_DISCRETE:-0} ./glxgears

.PHONY: run
run: all
	mkdir -p $$HOME/.cache
	gramine-vm glxgears

.PHONY: clean
clean:
	$(RM) *.manifest glxgears.o glxgears

.PHONY: distclean
distclean: clean
