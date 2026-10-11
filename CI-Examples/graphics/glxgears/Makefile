# SPDX-License-Identifier: BSD-3-Clause

# glxgears under gramine-vm with the Tirith graphics redirection (x86_64). See
# Documentation/devel/graphics/ for the host setup (QEMU fork, X proxy).

EGL_REDIRECT_DIR = $(abspath ../../../egl-redirect)

CFLAGS = -Wall -Wextra
LDLIBS = -lGL -lX11 -lm

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
CFLAGS += -g
else
GRAMINE_LOG_LEVEL = error
CFLAGS += -O3
endif

.PHONY: all
all: egl_redirect glxgears glxgears.manifest

.PHONY: egl_redirect
egl_redirect:
	$(MAKE) -C $(EGL_REDIRECT_DIR)

glxgears: glxgears.o
	$(CC) $^ -o $@ $(LDLIBS)

glxgears.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

glxgears.manifest: glxgears.manifest.template
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Degl_redirect=$(EGL_REDIRECT_DIR)/egl_redirect.so \
		$< $@
	gramine-manifest-check $@

# Without Gramine: the preload library presents through its own X connection.
.PHONY: run-native
run-native: all
	LD_PRELOAD=$(EGL_REDIRECT_DIR)/egl_redirect.so EGL_DISCRETE=$${EGL_DISCRETE:-0} ./glxgears

.PHONY: run
run: all
	mkdir -p $$HOME/.cache
	gramine-vm glxgears

.PHONY: clean
clean:
	$(RM) *.manifest glxgears.o glxgears

.PHONY: distclean
distclean: clean
