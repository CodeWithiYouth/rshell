"""Drive the GUI probe against the bursty SSE server and check that text is
revealed smoothly instead of jumping once per batch.

Run: python tests/ai_stream_e2e.py --exe build/modern/rshell_gui_tabs_probe.exe
     python tests/ai_stream_e2e.py --mode reasoning --exe ...   # 超长思考过程不卡帧
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

from sse_fake_server import full_text, reasoning_text, start_server


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--mode", choices=["text", "reasoning"], default="text")
    args = parser.parse_args()

    executable = Path(args.exe).resolve()
    workspace = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ai-stream-", dir=workspace / "build_tmp") as tmp:
        directory = Path(tmp)
        port, stop = start_server(mode=args.mode)
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
        env["RSHELL_AI_PERF"] = "1"
        env["RSHELL_NO_UI_SAVE"] = "1"
        if args.mode == "reasoning":
            env["RSHELL_AI_REASON_TEST"] = "1"
            env["RSHELL_AI_EXPECT_REASONING"] = str(len(reasoning_text()))
            marker = "REASON steps="
        else:
            env["RSHELL_AI_STREAM_TEST"] = "1"
            env["RSHELL_AI_EXPECT"] = str(len(full_text()))
            marker = "STREAM steps="
        try:
            result = subprocess.run(
                [str(executable)],
                env=env,
                cwd=workspace,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=180,
            )
        finally:
            stop()

        output = (result.stdout or "") + (result.stderr or "")
        for line in output.splitlines():
            if "STREAM" in line or "REASON" in line or "PASS" in line or "FAIL" in line or "delta" in line:
                print(line)
        if result.returncode != 0:
            print(output)
            return result.returncode
        if marker not in output:
            print(f"no {marker.strip()} result in output")
            return 1
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
