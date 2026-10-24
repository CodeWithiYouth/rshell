"""本地 SFTP 测试服务器（基于 paramiko）。

把 --root 指定的本地目录映射为远端根目录，从而可以端到端测试 rshell 的
上传、下载、删除、重命名、权限修改等需要写权限的场景。

仅用于本机测试：公钥认证默认接受任意公钥，不要用于生产环境。

用法：
    python tests/sftp_test_server.py --root tests/tmp/remote --port 2222
"""

from __future__ import annotations

import argparse
import errno
import os
import posixpath
import re
import socket
import subprocess
import threading
import time
import unicodedata

_ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[()*+][0-9A-Za-z]")

import paramiko


class LocalHandle(paramiko.SFTPHandle):
    """把 SFTP 文件句柄读写映射到本地文件对象。"""

    def __init__(self, flags, fobj):
        super().__init__(flags)
        self.fobj = fobj

    def read(self, offset, length):
        try:
            self.fobj.seek(offset)
            return self.fobj.read(length)
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)

    def write(self, offset, data):
        try:
            self.fobj.seek(offset)
            self.fobj.write(data)
            self.fobj.flush()
            return paramiko.SFTP_OK
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)

    def close(self):
        try:
            self.fobj.close()
        except OSError:
            pass
        return paramiko.SFTP_OK


def _err(exc):
    """把 OSError 转成 SFTP 错误码。"""
    return paramiko.SFTPServer.convert_errno(exc.errno or errno.EIO)


class LocalSFTPServer(paramiko.SFTPServerInterface):
    """把远端路径限制在 root 目录内，并映射到本地文件系统。"""

    def __init__(self, server, *args, **kwargs):
        self.root = os.path.abspath(kwargs.pop("root"))
        super().__init__(server, *args, **kwargs)

    # ------------------------------------------------------------ 路径
    def _local(self, path):
        rel = (path or "").lstrip("/")
        full = os.path.abspath(os.path.join(self.root, rel))
        if full != self.root and not full.startswith(self.root + os.sep):
            raise OSError(errno.EACCES, "路径越界")
        return full

    def canonicalize(self, path):
        # 远端始终以 / 为根，避免客户端把本地 cwd 带进来
        return posixpath.normpath("/" + (path or ""))

    # ------------------------------------------------------------ 查询
    def stat(self, path):
        try:
            return paramiko.SFTPAttributes.from_stat(os.stat(self._local(path)))
        except OSError as exc:
            return _err(exc)

    def lstat(self, path):
        try:
            return paramiko.SFTPAttributes.from_stat(os.lstat(self._local(path)))
        except OSError as exc:
            return _err(exc)

    def list_folder(self, path):
        try:
            entries = []
            with os.scandir(self._local(path)) as it:
                for entry in it:
                    try:
                        st = entry.stat()
                    except OSError:
                        continue
                    entries.append(paramiko.SFTPAttributes.from_stat(st, entry.name))
            return entries
        except OSError as exc:
            return _err(exc)

    def readlink(self, path):
        try:
            return os.readlink(self._local(path))
        except OSError as exc:
            return _err(exc)

    # ------------------------------------------------------------ 打开/修改
    def open(self, path, flags, attr):
        local = self._local(path)
        try:
            if flags & (os.O_WRONLY | os.O_RDWR):
                exists = os.path.exists(local)
                mode = "r+b" if exists and not (flags & os.O_TRUNC) else "w+b"
                fobj = open(local, mode)
                if flags & os.O_APPEND:
                    fobj.seek(0, os.SEEK_END)
            else:
                fobj = open(local, "rb")
            if attr is not None and attr.st_mode is not None and (flags & os.O_CREAT):
                # 仅在创建文件时应用权限；libssh2 以只读方式打开时也会带 mode=0，
                # 在 Windows 上会把已有文件误设为只读。
                perm = attr.st_mode & 0o7777
                if perm:
                    os.chmod(local, perm)
        except OSError as exc:
            return _err(exc)
        return LocalHandle(flags, fobj)

    def remove(self, path):
        try:
            os.remove(self._local(path))
            return paramiko.SFTP_OK
        except OSError as exc:
            return _err(exc)

    def rename(self, oldpath, newpath):
        try:
            os.rename(self._local(oldpath), self._local(newpath))
            return paramiko.SFTP_OK
        except OSError as exc:
            return _err(exc)

    def posix_rename(self, oldpath, newpath):
        try:
            os.replace(self._local(oldpath), self._local(newpath))
            return paramiko.SFTP_OK
        except OSError as exc:
            return _err(exc)

    def mkdir(self, path, attr):
        try:
            os.mkdir(self._local(path))
            if attr is not None and attr.st_mode is not None:
                os.chmod(self._local(path), attr.st_mode & 0o7777)
            return paramiko.SFTP_OK
        except OSError as exc:
            return _err(exc)

    def rmdir(self, path):
        try:
            os.rmdir(self._local(path))
            return paramiko.SFTP_OK
        except OSError as exc:
            return _err(exc)

    def chattr(self, path, attr):
        try:
            if attr is not None and attr.st_mode is not None:
                os.chmod(self._local(path), attr.st_mode)
            return paramiko.SFTP_OK
        except OSError as exc:
            return _err(exc)

    def symlink(self, target_path, path):
        try:
            os.symlink(target_path, self._local(path))
            return paramiko.SFTP_OK
        except OSError as exc:
            return _err(exc)


class TestSSHServer(paramiko.ServerInterface):
    """固定用户名/口令；公钥认证接受任意密钥（仅限本机测试）。"""

    def __init__(self, root, user, password, allow_public_key=True):
        self.root = root
        self.user = user
        self.password = password
        self.allow_public_key = allow_public_key

    def check_auth_password(self, username, password):
        if username == self.user and password == self.password:
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED

    def check_auth_publickey(self, username, key):
        if username != self.user or not self.allow_public_key:
            return paramiko.AUTH_FAILED
        return paramiko.AUTH_SUCCESSFUL

    def get_allowed_auths(self, username):
        return "password,publickey"

    def check_channel_request(self, kind, chanid):
        if kind == "session":
            return paramiko.OPEN_SUCCEEDED
        return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_subsystem_request(self, channel, name):
        # 交给 paramiko 默认实现：它会根据 set_subsystem_handler 启动 SFTP 线程
        return super().check_channel_subsystem_request(channel, name)

    def check_channel_exec_request(self, channel, command):
        threading.Thread(
            target=self._run_exec, args=(channel, command), daemon=True
        ).start()
        return True

    def check_channel_pty_request(
        self, channel, term, width, height, pixelwidth, pixelheight, modes
    ):
        # GUI 终端会先申请 PTY；这里接受任意尺寸
        self.pty_size = (int(width), int(height))
        return True

    def check_channel_shell_request(self, channel):
        threading.Thread(target=self._run_shell, args=(channel,), daemon=True).start()
        return True

    def check_channel_window_change_request(
        self, channel, width, height, pixelwidth, pixelheight
    ):
        self.pty_size = (int(width), int(height))
        return True

    def _run_shell(self, channel):
        """最小交互 shell：echo/pwd/uname/size/cat/boxes/exit，用于验证 PTY 与终端渲染。"""
        buffer = b""
        pending = b""
        try:
            channel.sendall(b"rshell-test-shell\r\n$ ")
            while True:
                data = channel.recv(4096)
                if not data:
                    break
                buffer += data
                # 处理回车/退格/Ctrl+C，支持逐字符回显
                while buffer:
                    if buffer.startswith(b"\x1b["):
                        end = buffer.find(b"~")
                        if end < 0:
                            break
                        buffer = buffer[end + 1 :]
                        continue
                    byte = buffer[:1]
                    buffer = buffer[1:]
                    if byte in (b"\r", b"\n"):
                        channel.sendall(b"\r\n")
                        command = pending.decode("utf-8", "replace").strip()
                        pending = b""
                        if command in ("exit", "logout", "quit"):
                            channel.sendall(b"bye\r\n")
                            channel.send_exit_status(0)
                            channel.shutdown_write()
                            channel.close()
                            return
                        if command.startswith("probe-sink "):
                            # 制造 SSH 窗口背压，验证客户端入队不阻塞 UI、数据顺序完整。
                            remaining = int(command.split()[1])
                            total = remaining
                            channel.sendall(b"sink-ready\r\n")
                            time.sleep(0.35)
                            valid = True
                            while remaining:
                                chunk = channel.recv(min(65536, remaining))
                                if not chunk:
                                    return
                                remaining -= len(chunk)
                                valid = valid and chunk == b"q" * len(chunk)
                            channel.sendall(f"sink:{total}:{'ok' if valid else 'bad'}\r\n".encode())
                        elif command.startswith("echo "):
                            channel.sendall(command[5:].encode("utf-8") + b"\r\n")
                        elif command == "pwd":
                            channel.sendall(self.root.encode("utf-8") + b"\r\n")
                        elif command == "uname":
                            channel.sendall(b"Linux rshell-test\r\n")
                        elif command == "size":
                            cols, rows = getattr(self, "pty_size", (0, 0))
                            channel.sendall(
                                f"{cols}x{rows}\r\n".encode("utf-8")
                            )
                        elif command == "boxes":
                            # 输出一个类 Ink/TUI 的满宽边框画面，用于检查渲染
                            cols, rows = getattr(self, "pty_size", (80, 24))
                            channel.sendall(self._frame(cols, rows))
                        elif command == "tui":
                            # 模拟 Bubble Tea / Ink：备用屏 + 绝对定位逐行重绘
                            cols, rows = getattr(self, "pty_size", (80, 24))
                            channel.sendall(
                                b"\x1b[?1049h\x1b[?25l"
                                + self._tui_frame(cols, rows)
                                + b"\x1b[?25h"
                            )
                        elif command.startswith("cat "):
                            name = command[4:].strip()
                            path = os.path.join(self.root, name)
                            try:
                                with open(path, "rb") as fh:
                                    channel.sendall(fh.read())
                            except OSError as exc:
                                channel.sendall(
                                    ("cat: " + str(exc)).encode("utf-8") + b"\r\n"
                                )
                        elif command:
                            channel.sendall(
                                ("sh: " + command + ": not found").encode("utf-8") + b"\r\n"
                            )
                        channel.sendall(b"$ ")
                    elif byte in (b"\x7f", b"\x08"):
                        if pending:
                            pending = pending[:-1]
                            channel.sendall(b"\x08 \x08")
                    elif byte == b"\x03":
                        pending = b""
                        channel.sendall(b"^C\r\n$ ")
                    elif byte >= b" ":
                        pending += byte
                        channel.sendall(byte)
        except Exception:  # noqa: BLE001 - 测试服务器只关注流程
            pass
        finally:
            try:
                channel.close()
            except Exception:  # noqa: BLE001
                pass

    @staticmethod
    def _tui_frame(cols, rows):
        """模拟 TUI 的整屏重绘：绝对定位 + 每行 ERASE + 写满整行（含最后一列）。"""
        cols = max(20, int(cols))
        rows = max(6, int(rows))
        inner = cols - 2

        def pad(text):
            visible = _ANSI_RE.sub("", text)
            width = 0
            for ch in visible:
                if ch == "\ufe0f":
                    width += 1
                    continue
                if ch == "\u200d":
                    continue
                wide = unicodedata.east_asian_width(ch) in ("W", "F") or (
                    ord(ch) >= 0x1F300
                ) or ch in ("\u2705", "\u26a1")
                width += 2 if wide else 1
            return text + " " * max(0, inner - width)

        lines = ["\u256d" + "\u2500" * inner + "\u256e"]
        lines.append("\u2502" + pad("  Cline-like TUI frame (alt screen)") + "\u2502")
        lines.append("\u251c" + "\u2500" * inner + "\u2524")
        lines.append("\u2502" + pad("  \u4f60\u597d\uff0c\u4e16\u754c  CJK \u5bf9\u9f50\u6d4b\u8bd5") + "\u2502")
        lines.append("\u2502" + pad("  emoji: \u26a0\ufe0f \u2705 \U0001F680") + "\u2502")
        lines.append("\u2570" + "\u2500" * inner + "\u256f")
        while len(lines) < rows - 2:
            lines.append("\u2502" + pad("  line %d" % (len(lines) + 1)) + "\u2502")
        lines = lines[: rows - 2]
        lines.append("\u2502" + pad("  > what can I do for you?") + "\u2502")
        lines.append("\u2570" + "\u2500" * inner + "\u256f")

        out = []
        for index, line in enumerate(lines, start=1):
            out.append("\x1b[%d;1H\x1b[2K" % index)
            out.append(line)
        # 反显状态栏
        status = " status: Plan | Auto-approve (Shift+Tab) "
        out.append("\x1b[%d;1H\x1b[7m%s\x1b[0m" % (rows, status.ljust(cols)))
        return "".join(out).encode("utf-8")

    @staticmethod
    def _frame(cols, rows):
        """生成一个满宽圆角边框 + 中文 + 颜色的画面（模拟 Ink 类 TUI）。"""
        cols = max(20, int(cols))
        inner = cols - 2

        def pad(text):
            # 宽度按“可见文本”计算（忽略 ANSI 转义），emoji + VS16 记为双宽
            visible = _ANSI_RE.sub("", text)
            width = 0
            for ch in visible:
                if ch == "\ufe0f":
                    width += 1
                    continue
                if ch == "\u200d":
                    continue
                wide = unicodedata.east_asian_width(ch) in ("W", "F") or (
                    ord(ch) >= 0x1F300
                ) or ch in ("\u2705", "\u26a1")
                width += 2 if wide else 1
            return text + " " * max(0, inner - width)

        def rule(left, right, fill="─"):
            return left + fill * inner + right

        lines = [
            "\x1b[2J\x1b[H",
            "\x1b[38;2;180;180;180m" + rule("╭", "╮"),
            "│" + pad("  rshell TUI render test") + "│",
            rule("├", "┤"),
            "│" + pad("  中文宽度对齐检查 中英混排 ABC") + "│",
            rule("╰", "╯"),
            "\x1b[0m",
            rule("╭", "╮"),
            "│" + pad("  What can I do for you?") + "│",
            rule("╰", "╯"),
            "\x1b[32m│\x1b[0m" + pad("  [ok] green border cell") + "\x1b[32m│\x1b[0m",
            "│" + pad("  emoji: \u26a0\ufe0f \u2705 \U0001F680 done") + "│",
            "│" + pad("  dec-graphics: " + "\x1b(0lqqqqqqk\x1b(B") + "│",
            rule("╭", "╮"),
            "│" + pad("  status: 中文状态栏 | cols=" + str(cols)) + "│",
            rule("╰", "╯"),
            "$ ",
        ]
        body = "\r\n".join(lines)
        return body.encode("utf-8")

    def _run_exec(self, channel, command):
        """在 root 目录下用系统 shell 执行命令，回传输出与退出码。"""
        if isinstance(command, bytes):
            command = command.decode("utf-8", "replace")
        try:
            proc = subprocess.run(
                command, shell=True, cwd=self.root, capture_output=True, timeout=120
            )
            if proc.stdout:
                channel.sendall(proc.stdout)
            if proc.stderr:
                channel.sendall_stderr(proc.stderr)
            channel.send_exit_status(proc.returncode)
        except Exception as exc:  # noqa: BLE001 - 测试服务器只需报错
            channel.sendall_stderr(str(exc).encode("utf-8", "replace"))
            channel.send_exit_status(1)
        finally:
            try:
                channel.shutdown_write()
            except Exception:  # noqa: BLE001
                pass
            channel.close()


def _handle_client(sock, root, user, password, host_key, strict_kex=True):
    transport = paramiko.Transport(sock, strict_kex=strict_kex)
    # paramiko 服务端按自身偏好选 KEX，而 libssh2 按客户端偏好选，两边可能不一致
    # （会在 KEX 中途报 MessageOrderError）。这里只提供一种双方都支持的算法。
    transport._preferred_kex = ("diffie-hellman-group14-sha256",)
    # 先注册 SFTP 子系统处理器，paramiko 的默认 check_channel_subsystem_request
    # 会据此创建并启动 SFTPServer 线程。
    transport.set_subsystem_handler(
        "sftp", paramiko.SFTPServer, sftp_si=LocalSFTPServer, root=root
    )
    try:
        transport.add_server_key(host_key)
        transport.start_server(server=TestSSHServer(root, user, password))
        while transport.is_active():
            time.sleep(0.2)
    except Exception:  # noqa: BLE001 - 客户端断开属正常情况
        pass
    finally:
        transport.close()


def start_server(
    root,
    host="127.0.0.1",
    port=0,
    user="tester",
    password="testpass",
    host_key_path=None,
    strict_kex=True,
):
    """启动服务器线程，返回 (host, port, stop_event, thread)。port=0 表示随机端口。"""
    os.makedirs(root, exist_ok=True)
    if host_key_path and os.path.exists(host_key_path):
        # 允许用任意类型的测试主机密钥（ed25519/ecdsa/rsa）。
        host_key = paramiko.PKey.from_path(host_key_path)
    else:
        host_key = paramiko.RSAKey.generate(2048)
        if host_key_path:
            os.makedirs(os.path.dirname(os.path.abspath(host_key_path)), exist_ok=True)
            host_key.write_private_key_file(host_key_path)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    sock.listen(16)
    sock.settimeout(0.5)
    actual_port = sock.getsockname()[1]

    stop = threading.Event()

    def accept_loop():
        while not stop.is_set():
            try:
                client, _ = sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            client.settimeout(None)  # accept 出的连接会继承监听 socket 的 0.5s 超时
            threading.Thread(
                target=_handle_client,
                args=(client, root, user, password, host_key, strict_kex),
                daemon=True,
            ).start()
        try:
            sock.close()
        except OSError:
            pass

    thread = threading.Thread(target=accept_loop, daemon=True)
    thread.start()
    return host, actual_port, stop, thread


def main():
    parser = argparse.ArgumentParser(description="rshell 本地 SFTP 测试服务器")
    parser.add_argument("--root", required=True, help="远端根目录（本地路径）")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2222)
    parser.add_argument("--user", default="tester")
    parser.add_argument("--password", default="testpass")
    parser.add_argument("--host-key", default=None, help="RSA 主机密钥文件（不存在则生成）")
    parser.add_argument("--no-strict-kex", action="store_true", help="关闭 strict KEX（诊断用）")
    parser.add_argument("--log-file", default=None, help="paramiko 调试日志文件")
    args = parser.parse_args()

    if args.log_file:
        paramiko.util.log_to_file(args.log_file, level="DEBUG")

    host, port, stop, _thread = start_server(
        root=args.root,
        host=args.host,
        port=args.port,
        user=args.user,
        password=args.password,
        host_key_path=args.host_key,
        strict_kex=not args.no_strict_kex,
    )
    print("READY {}:{}".format(host, port), flush=True)
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        stop.set()


if __name__ == "__main__":
    main()

