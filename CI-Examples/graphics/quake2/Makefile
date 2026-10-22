# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

# Yamagi Quake II under saguaro-vm with the Tirith graphics redirection (x86_64). Build the game
# first with ./setup-quake.sh; see dev docs: graphics/ for the host setup.

# The graphics preload library is built and installed with Gramine (-Dgraphics=enabled and a
# VM-type PAL); the manifest refers to it as gramine.libgraphics.
GRAPHICS_LIB = $(shell python3 -c "import graminelibos; print(graminelibos._CONFIG_PKGLIBDIR)")/libgraphics.so
Q2_DIR = $(abspath quake2_bin)
Q2_BIN = quake2_bin/release/quake2
Q2_ARGS ?= +vid_maxfps 999 +toggleconsole

# Input goes through the shared-memory ring in the comm page; 0 falls back to the X connection.
SHARED_INPUT ?= 1
# VULKAN=1: the Vulkan renderer ref_vk (built by setup-quake.sh) under saguaro-vm, presented through
# libgraphics.so's Vulkan layer (dev docs: vulkan/)
VULKAN ?= 0
LAYER_DIR = $(shell python3 -c "import graminelibos; print(graminelibos._CONFIG_PKGLIBDIR)")/vulkan


ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
else
SAGUARO_LOG_LEVEL = error
endif

MANIFEST_ARGS = \
	-Dlog_level=$(SAGUARO_LOG_LEVEL) \
	-Dq2_dir=$(Q2_DIR) \
	-Dhome=$(HOME) \
	-Dshared_input=$(SHARED_INPUT) \
	-Dvulkan=$(VULKAN) \
	-Degl_discrete=$${EGL_DISCRETE:-0} \
	-Dlayer_dir=$(LAYER_DIR)

.PHONY: all
all: quake2.manifest

$(Q2_BIN):
	@echo "Quake II is not built: run ./setup-quake.sh first" >&2; exit 1

.PHONY: FORCE
quake2.manifest: quake2.manifest.template $(Q2_BIN) FORCE
	mkdir -p ~/.yq2 
	saguaro-manifest $(MANIFEST_ARGS) $< $@
	saguaro-manifest-check $@

.PHONY: dirs
dirs:
	mkdir -p $(HOME)/.yq2 $(HOME)/.cache

# Without Gramine: the preload library presents through its own X connection. Quake II refuses to
# run as root, so as root (the usual case in the container) it is started in a user namespace that
# shows the same user as uid 1000; under Gramine the manifest's loader.uid does the same job.
ifeq ($(shell id -u),0)
NATIVE_AS = unshare --user --map-user=1000 --map-group=1000 env
else
NATIVE_AS = env
endif

.PHONY: run-native
run-native: $(Q2_BIN) dirs
	$(NATIVE_AS) LD_PRELOAD=$(GRAPHICS_LIB) EGL_DISCRETE=$${EGL_DISCRETE:-0} \
		SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null $(Q2_BIN) $(Q2_ARGS)

.PHONY: run
run: all dirs
	saguaro-vm quake2

# saguaro-direct on the host GPU (dev docs: graphics-direct/): stock Mesa with DRI3 by
# default; PRELOAD=1 uses the preload library instead (needs an install built with a VM-type PAL).
# The X server is the one of $(DISPLAY), reached through its Unix socket.
PRELOAD ?= 0
# The host's library directory of this architecture, and the NVIDIA driver's device nodes in the
# direct manifest when the host has them (NVIDIA=0 or 1 overrides).
ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)
NVIDIA ?= $(if $(wildcard /dev/nvidiactl),1,0)
X_DISPLAY_NUM = $(patsubst :%,%,$(firstword $(subst ., ,$(DISPLAY))))

quake2-direct.manifest: quake2-direct.manifest.template $(Q2_BIN) FORCE
		mkdir -p ~/.yq2
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
run-direct: quake2-direct.manifest dirs
	saguaro-direct quake2-direct $(Q2_ARGS)

# Quake II's own benchmark: plays the demo's recording as fast as possible and prints
# "633 frames, N seconds: N fps" (stop the game afterwards; it stays at the console).
.PHONY: run-direct-timedemo run-native-timedemo
run-direct-timedemo: quake2-direct.manifest dirs
	saguaro-direct quake2-direct +vid_maxfps 999 +timedemo 1 +map q2demo1.dm2
run-native-timedemo: $(Q2_BIN) dirs
	$(NATIVE_AS) $(if $(filter 1,$(PRELOAD)),LD_PRELOAD=$(GRAPHICS_LIB) EGL_DISCRETE=$${EGL_DISCRETE:-0}) \
		vblank_mode=0 SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null \
		$(Q2_BIN) +vid_maxfps 999 +timedemo 1 +map q2demo1.dm2

.PHONY: clean
clean:
	$(RM) *.manifest

.PHONY: distclean
distclean: clean
	$(RM) -r quake2_bin
