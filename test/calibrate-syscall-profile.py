#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Calibrate scripts/syscall-profile.py: profile distro builds of the example applications and
compare the verdicts with the outcomes known from the test documents.

Run it in the container (needs strace, gcc, curl, nc and the application packages; see
dev docs: syscall-profile/TESTING.md). Profiles go to .syscall-profiles/ (ignored).
The applications are the distro's, not the example directories' builds, so versions differ.
"""

import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("profile_tool", ROOT / "scripts/syscall-profile.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
OUT = ROOT / ".syscall-profiles"
WORK = pathlib.Path(tempfile.mkdtemp(prefix="calibrate-"))

# known outcome per loader: "pass", "fail" or None (not known; not scored)
APPS = {
    "helloworld": dict(known={"direct": "pass", "vm": "pass"}),
    "busybox": dict(cmd=["busybox", "uname", "-m"], known={"direct": "pass", "vm": "pass"}),
    "python": dict(cmd=["python3", "-c", "print('Hello World')"],
                   known={"direct": "pass", "vm": "pass"}),
    "sqlite": dict(cmd=["sqlite3", ":memory:",
                        "create table t(a); insert into t values(1); select * from t;"],
                   known={"direct": "pass", "vm": "pass"}),
    "bash": dict(cmd=["bash", "-c", "echo hi; ls / | cat > /dev/null; uname -m"],
                 known={"direct": "pass", "vm": "fail"}),
    "networks": dict(cmd=["python3", "-c",
                          "import socket\n"
                          "a=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); a.bind(('127.0.0.1',0))\n"
                          "b=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)\n"
                          "b.sendto(b'x',a.getsockname()); print(a.recvfrom(10))"],
                     known={"direct": "pass", "vm": "pass"}),
    "redis": dict(cmd=["redis-server", "--port", "6389", "--save", "", "--appendonly", "no",
                       "--daemonize", "no"],
                  drive="python3 -c \"import socket;s=socket.create_connection(('127.0.0.1',6389));"
                        "s.sendall(b'PING\\r\\nSET k v\\r\\nGET k\\r\\n');import time;time.sleep(.5);"
                        "print(s.recv(100))\"",
                  known={"direct": "pass", "vm": "pass"}),
    "memcached": dict(cmd=["memcached", "-u", "root", "-p", "11299"],
                      drive="printf 'set a 0 0 1\\r\\nx\\r\\nget a\\r\\nquit\\r\\n' | nc -q1 127.0.0.1 11299",
                      known={"direct": "pass", "vm": "pass"}),
    "nginx": dict(cmd=["nginx", "-c", str(WORK / "nginx.conf"), "-g", "daemon off; master_process off;"],
                  drive="curl -s -o /dev/null -w '%{http_code}\\n' http://127.0.0.1:8899/",
                  known={"direct": "pass", "vm": "pass"}),
    "lighttpd": dict(cmd=["lighttpd", "-D", "-f", str(WORK / "lighttpd.conf")],
                     drive="curl -s -o /dev/null -w '%{http_code}\\n' http://127.0.0.1:8898/",
                     known={"direct": "pass", "vm": "pass"}),
}


def prepare():
    (WORK / "www").mkdir()
    (WORK / "www/index.html").write_text("hello\n")
    (WORK / "nginx.conf").write_text(
        f"pid {WORK}/nginx.pid;\nerror_log {WORK}/nginx.err;\nevents {{}}\n"
        f"http {{ access_log off; client_body_temp_path {WORK}/b; proxy_temp_path {WORK}/p;\n"
        f"fastcgi_temp_path {WORK}/f; uwsgi_temp_path {WORK}/u; scgi_temp_path {WORK}/s;\n"
        f"server {{ listen 8899; root {WORK}/www; }} }}\n")
    (WORK / "lighttpd.conf").write_text(
        f'server.document-root = "{WORK}/www"\nserver.port = 8898\n'
        'server.modules = ()\nindex-file.names = ("index.html")\n')
    (WORK / "hello.c").write_text('#include <stdio.h>\nint main(){puts("Hello");return 0;}\n')
    subprocess.run(["gcc", "-O2", "-o", str(WORK / "hello"), str(WORK / "hello.c")], check=True)
    APPS["helloworld"]["cmd"] = [str(WORK / "hello")]


def main():
    prepare()
    matrix, evidence = tool.load_matrix()
    rules = tool.load_rules()
    OUT.mkdir(exist_ok=True)
    rows, mismatches = [], 0
    for name, app in APPS.items():
        profile = tool.do_profile(app["cmd"], OUT / f"{name}.json", app.get("drive"), 2.5)
        results = {loader: tool.check_loader(profile, loader, matrix, evidence, rules, 0.5)
                   for loader in ("direct", "vm")}
        for loader in ("direct", "vm"):
            result, known = results[loader], app["known"].get(loader)
            predicted = "fail" if result["verdict"] == "unlikely" else "pass"
            ok = known is None or predicted == known
            mismatches += not ok
            rows.append((name, loader, known or "?", result["verdict"], "ok" if ok else "MISMATCH",
                         ", ".join(f"{e['syscall']}" for e in result["blockers"][:4])))
        print(f"{name}: exit {profile['exit_status']}, drive {profile.get('drive_status')}, "
              f"{sum(c['count'] for c in profile['calls'].values())} calls, "
              f"processes {profile['multiprocess']['processes']}", flush=True)
    print("\napp         loader  known  verdict    result    blockers")
    for row in rows:
        print(f"{row[0]:<11} {row[1]:<7} {row[2]:<6} {row[3]:<10} {row[4]:<9} {row[5]}")
    print(f"\n{mismatches} mismatch(es) of {len(rows)} app/loader pairs")
    sys.exit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
