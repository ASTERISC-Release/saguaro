# Copyright (...)

# The example's udp.c is pure sockets, but this directory also ships it linked against
# the Tirith graphics stack (sharedgl + EGL/GL/X11) for the VM-PAL presentation plumbing.
# When that stack is not built (and its -L directory absent), build the plain-sockets
# variant: pass the graphics flags explicitly to get the linked variant back.
ifeq ($(wildcard ../../../sharedgl/build),)
CFLAGS ?= -Wall -Wextra
LDFLAGS ?= -lpthread
else
CFLAGS ?= -Wall -Wextra -I../../../sharedgl/inc/
LDFLAGS ?= -ldl -lEGL -lGL -lgbm -ldrm -lxcb -lxcb-dri3 -lxcb-present -lxcb-sync -lxshmfence -lm -L../../../sharedgl/build -lX11
endif
INCLUDES = -I/usr/include/libdrm -I./

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
CFLAGS += -g
else
GRAMINE_LOG_LEVEL = error
CFLAGS += -O3
endif

.PHONY: all
all: nw.manifest udp


udp: udp.o
	$(CC) udp.o -o udp $(LDFLAGS)

udp.o: udp.c
	$(CC) -fPIC $(INCLUDES) -g -c $< -o $@

# Correct manifest generation
nw.manifest: nw.manifest.template
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Dentrypoint="/udp" \
		-Denclave_size="256M" \
		-Darch_libdir=$(ARCH_LIBDIR) \
		$< $@
	gramine-manifest-check $@

.PHONY: clean
clean:
	rm -f *.manifest *.sig *.token udp udp.so udp.o


