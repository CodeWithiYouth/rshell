"""A tiny bursty SSE server that mimics a gateway which buffers tokens.

OpenAI-compatible: POST /v1/chat/completions streams `data: {...}` events.
Instead of one token every 20 ms it sends BATCHES of `CHARS_PER_BATCH` every
`BATCH_INTERVAL` seconds - the pattern measured in rshell's ai-perf.log.
"""
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BATCHES = 8
CHARS_PER_BATCH = 25
BATCH_INTERVAL = 0.35
MARKER = "【测试结束标记】"


def full_text() -> str:
    parts = []
    for i in range(BATCHES):
        parts.append(f"第{i + 1}段文字用于验证平滑输出，包含若干中文字符和 English 混排。")
    parts.append(MARKER)
    return "".join(parts)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # 安静一点
        pass

    def writeChunk(self, text: str):  # noqa: N802
        data = text.encode("utf-8")
        self.wfile.write(f"{len(data):X}\r\n".encode("ascii") + data + b"\r\n")
        self.wfile.flush()

    def do_POST(self):  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        if length:
            self.rfile.read(length)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        # 真实网关都是 chunked 流式；不带 length/close 才不会让 WinHTTP 攒到收尾
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        text = full_text()
        step = CHARS_PER_BATCH
        for start in range(0, len(text), step):
            batch = text[start:start + step]
            payload = {
                "id": "fake",
                "choices": [{"index": 0, "delta": {"content": batch}}],
            }
            self.writeChunk(f"data: {json.dumps(payload, ensure_ascii=False)}\n\n")
            time.sleep(BATCH_INTERVAL)
        done = {"id": "fake", "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}
        self.writeChunk(f"data: {json.dumps(done, ensure_ascii=False)}\n\n")
        self.writeChunk("data: [DONE]\n\n")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()
        self.close_connection = True


def start_server(port: int = 0):
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    opt = server.socket.getsockname()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return opt[1], server.server_close
