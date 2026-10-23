# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Build of the guest-side server of the vsock-loopback test (test/vm/network/test-vm-loopback.sh).
# static-pie: the VM PAL needs PIE binaries (the same requirement the CI-Examples under
# tools/run-vm-aarch64-examples.sh have); the manifest is rendered by `saguaro-test` (tests.toml).

CC ?= gcc
CFLAGS ?= -O2 -Wall

.PHONY: all clean
all: vsock_echo_server vsock_outbound_client vsock_udp_client vsock_guest_loopback

vsock_echo_server: vsock_echo_server.c
	$(CC) $(CFLAGS) -static-pie -o $@ $<

vsock_outbound_client: vsock_outbound_client.c
	$(CC) $(CFLAGS) -static-pie -o $@ $<

vsock_udp_client: vsock_udp_client.c
	$(CC) $(CFLAGS) -static-pie -o $@ $<

vsock_guest_loopback: vsock_guest_loopback.c
	$(CC) $(CFLAGS) -static-pie -pthread -o $@ $<

clean:
	$(RM) vsock_echo_server vsock_outbound_client vsock_echo_server.manifest \
		vsock_outbound_client.manifest vsock_udp_client vsock_udp_client.manifest \
		vsock_guest_loopback vsock_guest_loopback.manifest \
		build.ninja .ninja_deps .ninja_log
