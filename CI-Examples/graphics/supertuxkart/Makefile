# SPDX-License-Identifier: BSD-3-Clause

# SuperTuxKart under gramine-vm with the Tirith graphics redirection (x86_64). Build the game first
# with ./install-stk.sh; see Documentation/devel/graphics/ for the host setup.

EGL_BACKEND_DIR = $(abspath ../egl-backend)
STK_DIR = $(abspath .)
STK_BIN = stk-build/stk-code/cmake_build/bin/supertuxkart
STK_ARGS ?= --race-now --screensize=1920x1080 --seed=11 --track=abyss --kart=konqi
# Length in seconds of the automatic race run by `run-startup` and `run-native-startup`.
PROFILE_TIME ?= 1
# Input goes through the shared-memory ring in the comm page; 0 falls back to the X connection.
SHARED_INPUT ?= 1
STK_STARTUP_ARGS = --race-now --benchmark --profile-time=$(PROFILE_TIME) --screensize=1920x1080 \
	--windowed --seed=11 --track=abyss --kart=konqi

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

MANIFEST_ARGS = \
	-Dlog_level=$(GRAMINE_LOG_LEVEL) \
	-Degl_backend=$(EGL_BACKEND_DIR)/egl_backend.so \
	-Dstk_dir=$(STK_DIR) \
	-Dhome=$(HOME) \
	-Dprofile_time=$(PROFILE_TIME) \
	-Dshared_input=$(SHARED_INPUT)

.PHONY: all
all: egl_backend supertuxkart.manifest supertuxkart-startup.manifest

.PHONY: egl_backend
egl_backend:
	$(MAKE) -C $(EGL_BACKEND_DIR)

$(STK_BIN):
	@echo "SuperTuxKart is not built: run ./install-stk.sh first" >&2; exit 1

supertuxkart.manifest: supertuxkart.manifest.template $(STK_BIN) FORCE
	gramine-manifest $(MANIFEST_ARGS) $< $@
	gramine-manifest-check $@

# Automatic race of PROFILE_TIME seconds that prints the frame-rate statistics and exits.
.PHONY: FORCE
supertuxkart-startup.manifest: supertuxkart.manifest.template $(STK_BIN) FORCE
	gramine-manifest $(MANIFEST_ARGS) -Dstartup_probe=1 $< $@
	gramine-manifest-check $@

.PHONY: dirs
dirs:
	mkdir -p $(HOME)/.config $(HOME)/.local/share $(HOME)/.cache

# Without Gramine: the preload library presents through its own X connection. Run this once before
# the first VM run; it creates the game's configuration under ~/.config/supertuxkart.
.PHONY: run-native
run-native: egl_backend $(STK_BIN) dirs
	cd $(dir $(STK_BIN)) && LD_PRELOAD=$(EGL_BACKEND_DIR)/egl_backend.so \
		EGL_DISCRETE=$${EGL_DISCRETE:-0} SDL_AUDIODRIVER=dummy ./supertuxkart $(STK_ARGS)

.PHONY: run-native-startup
run-native-startup:
	$(MAKE) run-native STK_ARGS="$(STK_STARTUP_ARGS)"

.PHONY: run
run: all dirs
	gramine-vm supertuxkart

.PHONY: run-startup
run-startup: all dirs
	gramine-vm supertuxkart-startup

.PHONY: clean
clean:
	$(RM) *.manifest

.PHONY: distclean
distclean: clean
	$(RM) -r stk-build
