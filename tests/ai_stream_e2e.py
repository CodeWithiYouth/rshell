"""Drive the GUI probe against the bursty SSE server and check that text is
revealed smoothly instead of jumping once per batch.

Run: python tests/ai_stream_e2e.py --exe build/modern/rshell_gui_tabs_probe.exe
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

from sse_fake_server import MARKER, full_text, start_server


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    args = parser.parse_args()

    executable = Path(args.exe).resolve()
    workspace = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ai-stream-", dir=workspace / "build_tmp") as tmp:
        directory = Path(tmp)
        port, stop = start_server()
        ini = directory / "ai.ini"
        ini.write_text(
            "provider=\n"
            f"base_url=http://127.0.0.1:{port}/v1\n"
            "model=fake-model\n"
            "redact_secrets=1\n"
            "auto_approve=0\n"
            "save_sessions=0\n",
            encoding="utf-8",
        )

        env = os.environ.copy()
        env["RSHELL_AI_CONFIG"] = str(ini)
        env["RSHELL_AI_STREAM_TEST"] = "1"
        env["RSHELL_AI_PERF"] = "1"
        env["RSHELL_NO_UI_SAVE"] = "1"
        env["RSHELL_AI_EXPECT"] = str(len(full_text()))
        try:
            result = subprocess.run(
                [str(executable)],
                env=env,
                cwd=workspace,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=120,
            )
        finally:
            stop()

        output = (result.stdout or "") + (result.stderr or "")
        for line in output.splitlines():
            if "STREAM" in line or "PASS" in line or "FAIL" in line or "delta" in line:
                print(line)
        if result.returncode != 0:
            print(output)
            return result.returncode
        if "STREAM steps=" not in output:
            print("no STREAM result in output")
            return 1
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
