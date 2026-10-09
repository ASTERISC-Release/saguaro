# Copyright (...)

CFLAGS = -Wall -Wextra -I../../../sharedgl/inc/
LDFLAGS = -ldl -lEGL -lGL -lgbm -ldrm -lxcb -lxcb-dri3 -lxcb-present -lxcb-sync -lxshmfence -lm -L../../../sharedgl/build -lX11
INCLUDES = -I/usr/include/libdrm -I./

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
		$< $@
	gramine-manifest-check $@

.PHONY: clean
clean:
	rm -f *.manifest *.sig *.token udp udp.so udp.o


