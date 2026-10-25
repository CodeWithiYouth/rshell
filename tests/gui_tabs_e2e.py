"""Run the actual GUI multi-tab handlers against an isolated local SSH/SFTP server."""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile

import paramiko
from sftp_test_server import start_server


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--cwd-hooks", action="store_true")
    parser.add_argument("--cwd-delay", type=float, default=1.8)
    args = parser.parse_args()
    if args.cwd_hooks:
        os.environ["RSHELL_TEST_CWD_DELAY"] = str(args.cwd_delay)
    executable = Path(args.exe).resolve()
    workspace = Path(__file__).resolve().parents[1]
    temporary_parent = workspace / "build_tmp"
    temporary_parent.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gui-tabs-e2e-", dir=temporary_parent) as temporary:
        directory = Path(temporary)
        remote = directory / "remote"
        (remote / "alpha-dir").mkdir(parents=True)
        (remote / "beta-dir").mkdir()
        (remote / "alpha-dir" / "alpha.txt").write_bytes(b"alpha-download-content\n")
        profile = directory / "profile"
        (profile / ".ssh").mkdir(parents=True)
        (profile / ".rshell").mkdir()
        key = paramiko.RSAKey.generate(2048)
        key_path = directory / "server-key"
        key.write_private_key_file(str(key_path))
        client_key_path = directory / "client-key"
        paramiko.RSAKey.generate(2048).write_private_key_file(str(client_key_path))
        host, port, stop, thread = start_server(str(remote), host_key_path=str(key_path))
        (profile / ".ssh" / "known_hosts").write_text(
            f"[{host}]:{port} {key.get_name()} {key.get_base64()}\n", encoding="ascii"
        )
        upload = directory / "upload.bin"
        content = b"multi-tab-upload\n" * (384 * 1024)
        upload.write_bytes(content)
        download = directory / "download.txt"
        environment = os.environ.copy()
        environment["USERPROFILE"] = str(profile)
        environment["RSHELL_SESSIONS"] = str(profile / "sessions.ini")
        environment["RSHELL_NO_CWD_SYNC"] = "1"
        if args.cwd_hooks:
            environment.pop("RSHELL_NO_CWD_SYNC", None)
            environment["RSHELL_TEST_CWD_HOOKS"] = "1"
        environment["RSHELL_TABS_PREVIEW"] = str(temporary_parent / "session-tabs-preview.bmp")
        environment["TEMP"] = str(directory)
        environment["TMP"] = str(directory)
        try:
            result = subprocess.run([str(executable), str(port), str(upload), str(download), str(client_key_path)],
                                    env=environment, cwd=workspace, capture_output=True,
                                    text=True, encoding="utf-8", errors="replace", timeout=100)
            print(result.stdout, end="")
            if result.returncode:
                raise RuntimeError(result.stderr or f"GUI probe exited {result.returncode}")
            assert (remote / "beta-dir" / "upload.bin").read_bytes() == content
            assert download.read_bytes() == b"alpha-download-content\n"
            if args.cwd_hooks:
                log = (profile / ".rshell" / "gui.log").read_text(encoding="utf-8")
                times = re.findall(r"目录跟随初始化完成（(\d+) ms）", log)
                assert len(times) >= 2
                print("PASS: acknowledged OSC7 initialization latency (ms): " + ", ".join(times[:2]))
            print("PASS: real concurrent SSH, Chinese input, independent SFTP, background transfers, reconnect and pending-close")
        finally:
            stop.set()
            thread.join(timeout=3)


if __name__ == "__main__":
    main()
