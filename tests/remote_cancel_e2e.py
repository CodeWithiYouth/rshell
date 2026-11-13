"""Local-only stalled SFTP and AI cancellation regression (requires paramiko)."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
import threading
import time

import sftp_test_server as server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="build/modern/rshell_remote_cancel_probe.exe")
    args = parser.parse_args()
    executable = str(Path(args.exe).resolve())
    with tempfile.TemporaryDirectory(prefix="rshell-cancel-") as temporary:
        root = Path(temporary)
        (root / "large.bin").write_bytes(b"transfer-data" * (1024 * 1024))
        (root / "small.txt").write_text("still-connected", encoding="utf-8")
        (root / "reader.txt").write_text("reader-cleanup", encoding="utf-8")
        (root / "slow-list").mkdir()
        for name in ("slow-read.bin", "slow-timeout.bin"):
            (root / name).write_bytes(b"delayed-data" * 32768)

        original_read = server.LocalHandle.read
        original_write = server.LocalHandle.write
        original_init = server.LocalHandle.__init__
        original_close = server.LocalHandle.close
        original_list = server.LocalSFTPServer.list_folder
        original_exec = server.TestSSHServer._run_exec
        handles = []
        active = set()
        activity = threading.Condition()

        def handle_init(handle, flags, file_object):
            original_init(handle, flags, file_object)
            handles.append(handle)

        def handle_close(handle):
            result = original_close(handle)
            if Path(handle.fobj.name).name == "reader.txt":
                (root / "reader-closed").touch()
            return result

        def delayed(operation, handle, *arguments):
            with activity:
                active.add(threading.get_ident())
            try:
                time.sleep(5)
                return operation(handle, *arguments)
            finally:
                with activity:
                    active.discard(threading.get_ident())
                    activity.notify_all()

        def delayed_read(handle, offset, length):
            if Path(handle.fobj.name).name.startswith("slow-"):
                (root / "read-entered").touch()
                return delayed(original_read, handle, offset, length)
            return original_read(handle, offset, length)

        def delayed_write(handle, offset, data):
            if Path(handle.fobj.name).name == "slow-write.bin":
                (root / "write-entered").touch()
                return delayed(original_write, handle, offset, data)
            return original_write(handle, offset, data)

        def delayed_list(instance, path):
            if path == "/slow-list":
                (root / "list-entered").touch()
                return delayed(original_list, instance, path)
            return original_list(instance, path)

        def command(server_instance, channel, text):
            if isinstance(text, bytes):
                text = text.decode("utf-8")
            if text == "RSHELL_TEST_LONG_EXEC":
                (root / "exec-entered").touch()
                time.sleep(5)
            elif text != "RSHELL_TEST_FAST_EXEC":
                return original_exec(server_instance, channel, text)
            try:
                channel.sendall(b"fast-ok")
                channel.send_exit_status(0)
            except (OSError, EOFError):
                pass
            finally:
                channel.close()

        server.LocalHandle.__init__ = handle_init
        server.LocalHandle.close = handle_close
        server.LocalHandle.read = delayed_read
        server.LocalHandle.write = delayed_write
        server.LocalSFTPServer.list_folder = delayed_list
        server.TestSSHServer._run_exec = command
        host, port, stop, thread = server.start_server(str(root))
        try:
            result = subprocess.run(
                [executable, host, str(port), "tester", "testpass", str(root)],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=40,
            )
            print(result.stdout, end="")
            if result.returncode and result.stderr:
                print(result.stderr, end="")
            if result.returncode:
                raise SystemExit(result.returncode)
        finally:
            stop.set()
            thread.join(timeout=2)
            # The client deliberately leaves while the server's fake IO sleeps.
            # Wait for those handlers before removing fixtures on Windows.
            with activity:
                activity.wait_for(lambda: not active, timeout=6)
            for handle in handles:
                handle.close()


if __name__ == "__main__":
    main()
