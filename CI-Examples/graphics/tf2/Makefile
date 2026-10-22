# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

# Team Fortress 2 (native Linux client) under gramine-vm with the Tirith graphics redirection
# (x86_64). Ported from Tirith but not yet run in this tree; see README.md for what it needs.

TF2_DIR ?= $(CURDIR)/game
TF2_STEAM_HOME ?= $(if $(TF2_NATIVE_HOME),$(TF2_NATIVE_HOME),$(HOME))
TF2_DISPLAY ?= 2:0
TF2_WIDTH ?= 1920
TF2_HEIGHT ?= 1080
# Input over the X connection by default (as in Tirith); 1 selects the shared input ring.
SHARED_INPUT ?= 0

.PHONY: all
all: tf2.manifest

.PHONY: check-install
check-install:
	@test -x "$(TF2_DIR)/tf_linux64" || { \
		echo "error: $(TF2_DIR)/tf_linux64 is missing" >&2; \
		echo "run ./setup-tf2.sh STEAM_USERNAME, or set TF2_DIR to an existing Linux TF2 install" >&2; \
		exit 1; \
	}
	@test -f "$(TF2_DIR)/tf/bin/linux64/client.so" || { \
		echo "error: the TF2 Linux client depot is incomplete (missing tf/bin/linux64/client.so)" >&2; \
		exit 1; \
	}

# Frame-time collector preloaded next to the graphics library.
fps_metrics.so: fps_metrics.c
	$(CC) -O2 -fPIC -shared -Wall -Wextra -Werror -o $@ $< -ldl -pthread

.PHONY: FORCE
FORCE:

tf2.manifest: tf2.manifest.template fps_metrics.so FORCE
	gramine-manifest \
		-Dtf2_dir="$(realpath $(TF2_DIR))" \
		-Dsteam_home="$(realpath $(TF2_STEAM_HOME))" \
		-Dintegration_dir="$(CURDIR)" \
		-Ddisplay="$(TF2_DISPLAY)" \
		-Dwidth="$(TF2_WIDTH)" \
		-Dheight="$(TF2_HEIGHT)" \
		-Dshared_input="$(SHARED_INPUT)" \
		$< $@
	gramine-manifest-check $@

.PHONY: run
run: check-install
	SAGUARO_RAM_SIZE="$${SAGUARO_RAM_SIZE:-16G}" \
	SAGUARO_CPU_NUM="$${SAGUARO_CPU_NUM:-1}" \
	./run-vm.sh

.PHONY: clean
clean:
	$(RM) tf2.manifest fps_metrics.so
