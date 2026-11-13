"""Local-only throttled transfer/interaction regression (requires paramiko)."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

import sftp_test_server as server


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            value.update(chunk)
    return value.hexdigest()


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="build/modern/rshell_transfer_interaction_probe.exe")
    parser.add_argument("--tight-window", action="store_true",
                        help="stress partial SSH packets with a 32 KiB window and 4 KiB packets")
    args = parser.parse_args()
    executable = str(Path(args.exe).resolve())
    with tempfile.TemporaryDirectory(prefix="rshell-interaction-") as temporary:
        fixture = Path(temporary)
        root, client = fixture / "remote", fixture / "client"
        root.mkdir()
        client.mkdir()
        block = bytes(range(256)) * 4096
        for path in (root / "source.bin", client / "source.bin"):
            with path.open("wb") as output:
                for _ in range(4):
                    output.write(block)
        (root / "small.txt").write_text("interaction-ok", encoding="utf-8")
        (root / "empty.txt").write_bytes(b"")
        (root / "small-tail.bin").write_bytes(bytes(range(256)) * 256 + b"Z")
        (client / "queue.txt").write_text("queued-transfer", encoding="utf-8")
        (client / "tree" / "nested").mkdir(parents=True)
        (client / "tree" / "config.txt").write_text("directory-content", encoding="utf-8")
        (client / "tree" / "nested" / "payload.bin").write_bytes(block * 2)

        original_read = server.LocalHandle.read
        original_write = server.LocalHandle.write
        original_init = server.LocalHandle.__init__
        original_server_init = server.TestSSHServer.__init__
        original_transport_init = server.paramiko.Transport.__init__
        original_exec = server.TestSSHServer._run_exec
        handles = []
        connections = 0
        lock = threading.Lock()
        bytes_per_second = 1.5 * 1024 * 1024

        def handle_init(handle, flags, file_object):
            original_init(handle, flags, file_object)
            handles.append(handle)

        def connection_init(instance, *arguments, **keywords):
            nonlocal connections
            with lock:
                connections += 1
            original_server_init(instance, *arguments, **keywords)

        def transport_init(instance, sock, *arguments, **keywords):
            if args.tight_window:
                keywords.setdefault("default_window_size", 32768)
                keywords.setdefault("default_max_packet_size", 4096)
            original_transport_init(instance, sock, *arguments, **keywords)

        def delayed_read(handle, offset, length):
            if Path(handle.fobj.name).suffix == ".bin":
                time.sleep(length / bytes_per_second)
            return original_read(handle, offset, length)

        def delayed_write(handle, offset, data):
            if Path(handle.fobj.name).suffix == ".bin":
                time.sleep(len(data) / bytes_per_second)
            return original_write(handle, offset, data)

        def command(instance, channel, text):
            if isinstance(text, bytes):
                text = text.decode("utf-8", "replace")
            if text == "RSHELL_TRANSFER_WAIT":
                (root / "interaction-entered").touch()
                time.sleep(0.6)
                output = b"command-ok"
            elif text == "RSHELL_TRANSFER_CANCEL_DURING_EXEC":
                (root / "transfer-command-entered").touch()
                time.sleep(0.25)
                output = b"command-ok"
            elif text == "RSHELL_TRANSFER_CANCEL_UPLOAD_DURING_EXEC":
                (root / "upload-command-entered").touch()
                time.sleep(0.25)
                output = b"command-ok"
            elif text == "RSHELL_TRANSFER_FAST":
                output = b"command-ok"
            elif "##HOST" in text and "##CPU2" in text:
                output = (b"##HOST\nfixture\n##OS\nTest\n##UP\n100\n##LOAD\n0 0 0\n"
                          b"##CORES\n2\n##CPU\ncpu 10 0 10 80\n"
                          b"##CPU2\ncpu 11 0 11 88\n##MEM\nMemTotal: 1000 kB\n"
                          b"MemAvailable: 600 kB\n##DISK\n/dev/test 1000 400 600 40% /\n##END\n")
            else:
                return original_exec(instance, channel, text)
            try:
                channel.sendall(output)
                channel.send_exit_status(0)
            except (OSError, EOFError):
                pass
            finally:
                channel.close()

        server.LocalHandle.__init__ = handle_init
        server.LocalHandle.read = delayed_read
        server.LocalHandle.write = delayed_write
        server.TestSSHServer.__init__ = connection_init
        server.paramiko.Transport.__init__ = transport_init
        server.TestSSHServer._run_exec = command
        host, port, stop, thread = server.start_server(str(root))
        started = time.monotonic()
        try:
            probe = [executable, host, str(port), "tester", "testpass", str(fixture)]
            if args.tight_window:
                probe.append("--tight-socket")
            result = subprocess.run(
                probe,
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60,
            )
            print(result.stdout, end="")
            if result.returncode:
                print(result.stderr, end="")
                raise SystemExit(result.returncode)
            reference = digest(client / "source.bin")
            for path in (root / "uploaded.bin", root / "resumed-upload.bin",
                         client / "downloaded.bin", client / "resumed.bin"):
                assert path.stat().st_size == 4 * 1024 * 1024, str(path)
                assert digest(path) == reference, f"SHA-256 mismatch: {path}"
            for source in (client / "tree").rglob("*"):
                if source.is_file():
                    relative = source.relative_to(client / "tree")
                    for base in (root / "uploaded-tree", client / "downloaded-tree"):
                        assert digest(base / relative) == digest(source), f"tree mismatch: {relative}"
            assert (root / "queued-upload.txt").read_text(encoding="utf-8") == "queued-transfer"
            assert (client / "queued-download.txt").read_text(encoding="utf-8") == "interaction-ok"
            assert connections == 1, f"expected one SSH connection, got {connections}"
            mode = "tight window" if args.tight_window else "default window"
            print(f"PASS: SHA-256, full byte counts, recursive content and one SSH connection ({mode}, {time.monotonic() - started:.1f}s)")
        finally:
            stop.set()
            thread.join(timeout=2)
            for handle in handles:
                handle.close()


if __name__ == "__main__":
    main()
