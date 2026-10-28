#!/usr/bin/env python3
"""proxy_test.py - 用本地 SOCKS5 / HTTP CONNECT 代理验证 rshell 的代理连接

自带一个最小 SSH 服务器（复用 sftp_test_server.py）和两个要求认证的代理，
然后调用 rshell.exe 经代理执行远端命令。

用法:
  python tests/proxy_test.py [--exe build\\modern\\rshell.exe] [--keep]
"""
import argparse
import base64
import os
import select
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
USER = "tester"
PASSWORD = "testpass"
PROXY_USER = "puser"
PROXY_PASS = "ppass"


def _pump(a, b):
    """双向转发，直到任意一侧关闭。"""
    sockets = [a, b]
    try:
        while True:
            readable, _, errored = select.select(sockets, [], sockets, 30)
            if errored:
                return
            if not readable:
                return
            for src in readable:
                data = src.recv(65536)
                if not data:
                    return
                dst = b if src is a else a
                dst.sendall(data)
    finally:
        for s in sockets:
            try:
                s.close()
            except OSError:
                pass


def _read_until(sock, marker, limit=8192):
    data = b""
    while marker not in data and len(data) < limit:
        chunk = sock.recv(1)
        if not chunk:
            break
        data += chunk
    return data


class Socks5Proxy(threading.Thread):
    """最小 SOCKS5：只接受用户名/密码认证。"""

    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.connections = 0

    def run(self):
        while True:
            try:
                client, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._handle, args=(client,), daemon=True).start()

    def _handle(self, client):
        try:
            head = client.recv(2)
            methods = client.recv(head[1]) if len(head) == 2 else b""
            if 0x02 not in methods:
                client.sendall(b"\x05\xff")
                return
            client.sendall(b"\x05\x02")
            auth = client.recv(2)
            user = client.recv(auth[1])
            plen = client.recv(1)[0]
            password = client.recv(plen)
            if user.decode() != PROXY_USER or password.decode() != PROXY_PASS:
                client.sendall(b"\x01\x01")
                return
            client.sendall(b"\x01\x00")

            req = client.recv(4)
            atyp = req[3]
            if atyp == 0x03:
                length = client.recv(1)[0]
                host = client.recv(length).decode()
            elif atyp == 0x01:
                host = socket.inet_ntoa(client.recv(4))
            else:
                client.sendall(b"\x05\x08\x00\x01" + b"\x00" * 6)
                return
            port = int.from_bytes(client.recv(2), "big")

            upstream = socket.create_connection((host, port), timeout=10)
            client.sendall(b"\x05\x00\x00\x01" + b"\x00" * 4 + b"\x00" * 2)
            self.connections += 1
            _pump(client, upstream)
        except Exception:  # noqa: BLE001 - 测试代理，出错就断开
            try:
                client.close()
            except OSError:
                pass


class HttpConnectProxy(threading.Thread):
    """最小 HTTP CONNECT：要求 Basic 认证。"""

    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.connections = 0

    def run(self):
        while True:
            try:
                client, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._handle, args=(client,), daemon=True).start()

    def _handle(self, client):
        try:
            head = _read_until(client, b"\r\n\r\n")
            lines = head.decode("latin1").split("\r\n")
            if not lines or not lines[0].startswith("CONNECT "):
                client.sendall(b"HTTP/1.1 405 Method Not Allowed\r\n\r\n")
                return
            expected = "Basic " + base64.b64encode(
                (PROXY_USER + ":" + PROXY_PASS).encode()).decode()
            if not any(line.lower().startswith("proxy-authorization:") and
                       line.split(":", 1)[1].strip() == expected for line in lines):
                client.sendall(b"HTTP/1.1 407 Proxy Authentication Required\r\n\r\n")
                return
            target = lines[0].split(" ")[1]
            host, _, port = target.rpartition(":")
            upstream = socket.create_connection((host, int(port)), timeout=10)
            client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            self.connections += 1
            _pump(client, upstream)
        except Exception:  # noqa: BLE001
            try:
                client.close()
            except OSError:
                pass


def run(exe, ssh_port, proxy_url, command, timeout=90):
    cmd = [
        exe, "--insecure", "--no-progress",
        "-H", "127.0.0.1", "-p", str(ssh_port),
        "-u", USER, "--password", PASSWORD,
        "--proxy", proxy_url,
        "exec", command,
    ]
    return subprocess.run(cmd, capture_output=True, timeout=timeout,
                          encoding="utf-8", errors="replace")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(Path("build/modern/rshell.exe")))
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    exe = str(Path(args.exe).resolve())
    if not os.path.isfile(exe):
        print("找不到 rshell.exe：" + exe, file=sys.stderr)
        return 2

    sys.path.insert(0, str(HERE))
    from sftp_test_server import start_server

    root = Path(tempfile.mkdtemp(prefix="rshell-proxy-"))
    host, ssh_port, stop, _thread = start_server(str(root), host="127.0.0.1", port=0,
                                                 user=USER, password=PASSWORD)
    print("SSH 测试服务器 127.0.0.1:{}，目录 {}".format(ssh_port, root))

    socks = Socks5Proxy()
    socks.start()
    http = HttpConnectProxy()
    http.start()
    print("SOCKS5 代理 127.0.0.1:{}；HTTP CONNECT 代理 127.0.0.1:{}".format(socks.port, http.port))

    failures = 0
    try:
        cases = [
            ("socks5 带认证",
             "socks5://{}:{}@127.0.0.1:{}".format(PROXY_USER, PROXY_PASS, socks.port)),
            ("http connect 带认证",
             "http://{}:{}@127.0.0.1:{}".format(PROXY_USER, PROXY_PASS, http.port)),
        ]
        for name, url in cases:
            result = run(exe, ssh_port, url, "echo proxied-ok")
            ok = result.returncode == 0 and "proxied-ok" in result.stdout
            print("  {}  {}  （退出码 {}）".format("PASS" if ok else "FAIL", name,
                                                  result.returncode))
            if not ok:
                failures += 1
                print("        stdout: " + result.stdout.strip()[:300])
                print("        stderr: " + result.stderr.strip()[:300])

        # 错误的代理端口必须失败，且不能悄悄退回直连
        result = run(exe, ssh_port, "socks5://127.0.0.1:1", "echo should-not-run")
        ok = result.returncode != 0 and "should-not-run" not in result.stdout
        print("  {}  代理不可用时拒绝连接".format("PASS" if ok else "FAIL"))
        if not ok:
            failures += 1
            print("        stdout: " + result.stdout.strip()[:300])

        # 认证错误必须失败
        result = run(exe, ssh_port,
                     "socks5://{}:{}@127.0.0.1:{}".format(PROXY_USER, "wrong", socks.port),
                     "echo should-not-run")
        ok = result.returncode != 0 and "should-not-run" not in result.stdout
        print("  {}  代理认证错误被拒绝".format("PASS" if ok else "FAIL"))
        if not ok:
            failures += 1
    finally:
        stop.set()
        socks.sock.close()
        http.sock.close()
        if not args.keep:
            import shutil
            shutil.rmtree(root, ignore_errors=True)

    print("PASS" if failures == 0 else "FAIL")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
