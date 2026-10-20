#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

"""Unit checks for the VM relay handshake and outbound policy."""

import argparse
import asyncio
import errno
import socket
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

from graminelibos.vsock_relay import (
    CONNECT_MAGIC,
    CONNECT_OP,
    CONNECT_STATUS_ERROR,
    CONNECT_STATUS_OK,
    CONNECT_TCP,
    CONNECT_VERSION,
    NetworkPolicy,
    Stats,
    load_config,
    serve_guest_connection,
)


def request(address, port):
    raw_address = bytes(int(part) for part in address.split('.'))
    return (CONNECT_MAGIC + bytes((CONNECT_VERSION, CONNECT_OP, CONNECT_TCP, 4)) +
            port.to_bytes(2, 'big') + len(raw_address).to_bytes(2, 'big') + raw_address)


class FakeWriter:
    def __init__(self, peername):
        self.peername = peername
        self.data = bytearray()
        self.closed = False

    def get_extra_info(self, name):
        return self.peername if name == 'peername' else None

    def write(self, data):
        self.data.extend(data)

    async def drain(self):
        return None

    def can_write_eof(self):
        return True

    def write_eof(self):
        return None

    def close(self):
        self.closed = True


class RelayTest(unittest.TestCase):
    def test_default_allow_and_deny_rules(self):
        self.assertFalse(NetworkPolicy().denied('192.0.2.7', 443))
        policy = NetworkPolicy(deny=['10.0.0.0/8', '2001:db8::/32', 'tcp:25'])
        self.assertTrue(policy.denied('10.2.3.4', 80))
        self.assertTrue(policy.denied('2001:db8::5', 443))
        self.assertTrue(policy.denied('192.0.2.7', 25))
        self.assertFalse(policy.denied('192.0.2.7', 443))
        self.assertTrue(NetworkPolicy('deny').denied('192.0.2.7', 443))

    def test_manifest_policy_and_inbound_rules(self):
        with tempfile.TemporaryDirectory() as tempdir:
            path = Path(tempdir) / 'app.manifest'
            path.write_text('''
[vm.net]
outbound = "deny"
deny = ["192.0.2.0/24", "tcp:25"]
inbound = [{guest = 8080, host = 18080, bind = "127.0.0.1"}]
''')
            rules, policy = load_config(argparse.Namespace(config=str(path), inbound=[]))
        self.assertEqual([(rule.guest, rule.host, rule.bind) for rule in rules],
                         [(8080, 18080, '127.0.0.1')])
        self.assertTrue(policy.denied('198.51.100.1', 443))

    def test_outbound_connect_and_reply(self):
        async def run():
            guest_reader = asyncio.StreamReader()
            guest_reader.feed_data(request('192.0.2.9', 9000) + b'request')
            guest_reader.feed_eof()
            guest_writer = FakeWriter((1510, 32100))

            host_reader = asyncio.StreamReader()
            host_reader.feed_data(b'reply')
            host_reader.feed_eof()
            host_writer = FakeWriter(('192.0.2.9', 9000))

            with patch('graminelibos.vsock_relay.asyncio.open_connection',
                       new=AsyncMock(return_value=(host_reader, host_writer))) as connect:
                await serve_guest_connection(guest_reader, guest_writer, 1510, Stats())

            connect.assert_awaited_once_with('192.0.2.9', 9000, family=socket.AF_INET)
            self.assertEqual(guest_writer.data[:4], CONNECT_MAGIC)
            self.assertEqual(guest_writer.data[5], CONNECT_STATUS_OK)
            self.assertEqual(bytes(host_writer.data), b'request')
            self.assertIn(b'reply', guest_writer.data)
            self.assertTrue(guest_writer.closed)
            self.assertTrue(host_writer.closed)

        asyncio.run(run())

    def test_denied_connect_returns_eacces_without_dialing(self):
        async def run():
            guest_reader = asyncio.StreamReader()
            guest_reader.feed_data(request('192.0.2.9', 443))
            guest_reader.feed_eof()
            guest_writer = FakeWriter((1510, 32100))

            with patch('graminelibos.vsock_relay.asyncio.open_connection',
                       new=AsyncMock()) as connect:
                await serve_guest_connection(guest_reader, guest_writer, 1510, Stats(),
                                             NetworkPolicy(deny=['tcp:443']))

            connect.assert_not_awaited()
            self.assertEqual(guest_writer.data[:4], CONNECT_MAGIC)
            self.assertEqual(guest_writer.data[5], CONNECT_STATUS_ERROR)
            self.assertEqual(int.from_bytes(guest_writer.data[6:8], 'big'), errno.EACCES)

        asyncio.run(run())


if __name__ == '__main__':
    unittest.main()
