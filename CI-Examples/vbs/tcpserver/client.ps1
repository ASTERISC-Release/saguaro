# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Gramine contributors
#
# A client for the TCP server example: connects to 127.0.0.1:18080, sends a line, prints the answer.
#   .\client.ps1 [-Message text] [-Port 18080] [-Count 1]
param([string]$Message = 'hello', [int]$Port = 18080, [int]$Count = 1)
for ($i = 1; $i -le $Count; $i++) {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', $Port)
    $s = $c.GetStream()
    $b = [Text.Encoding]::ASCII.GetBytes("$Message $i")
    $s.Write($b, 0, $b.Length)
    $r = New-Object IO.StreamReader($s)
    $r.ReadToEnd()
    $c.Close()
}
