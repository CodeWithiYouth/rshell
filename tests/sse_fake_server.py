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

# 思考过程：长文本 + 大块推送，用来检验面板不会因为重排整段而卡住
REASONING_SEGMENTS = 40
REASONING_CHARS_PER_SEGMENT = 500
REASONING_INTERVAL = 0.05


def full_text() -> str:
    parts = []
    for i in range(BATCHES):
        parts.append(f"第{i + 1}段文字用于验证平滑输出，包含若干中文字符和 English 混排。")
    parts.append(MARKER)
    return "".join(parts)


def reasoning_text() -> str:
    line = "先确认端口是否在监听，再检查进程和配置，顺便看一眼最近的日志有没有异常。\n"
    segments = []
    for i in range(REASONING_SEGMENTS):
        segments.append(f"[{i + 1}] " + line * 4)  # 每段 4 行，贴近真实的长思考
    return "".join(segments)


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

        mode = getattr(self.server, "mode", "text")
        if mode in ("limit", "approval"):
            name = "name_session" if mode == "limit" else "run_command"
            arguments = '{"title":"测试"}' if mode == "limit" else '{"command":"echo test"}'
            payload = {"choices": [{"index": 0, "delta": {"tool_calls": [{
                "index": 0, "id": "call-state", "type": "function",
                "function": {"name": name, "arguments": arguments}}]},
                "finish_reason": "tool_calls"}]}
            self.writeChunk(f"data: {json.dumps(payload, ensure_ascii=False)}\n\n")
            self.writeChunk("data: [DONE]\n\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
            self.close_connection = True
            return
        if mode == "reasoning":
            self.streamReasoning()
            return

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

    def streamReasoning(self):  # noqa: N802
        """先推一大段 reasoning_content，再给一句正式回答。"""
        text = reasoning_text()
        for start in range(0, len(text), REASONING_CHARS_PER_SEGMENT):
            batch = text[start:start + REASONING_CHARS_PER_SEGMENT]
            payload = {
                "id": "fake",
                "choices": [{"index": 0, "delta": {"reasoning_content": batch}}],
            }
            self.writeChunk(f"data: {json.dumps(payload, ensure_ascii=False)}\n\n")
            time.sleep(REASONING_INTERVAL)
        payload = {
            "id": "fake",
            "choices": [{"index": 0, "delta": {"content": "思考完毕。"}}],
        }
        self.writeChunk(f"data: {json.dumps(payload, ensure_ascii=False)}\n\n")
        done = {"id": "fake", "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}
        self.writeChunk(f"data: {json.dumps(done, ensure_ascii=False)}\n\n")
        self.writeChunk("data: [DONE]\n\n")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()
        self.close_connection = True


def start_server(port: int = 0, mode: str = "text"):
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.mode = mode
    opt = server.socket.getsockname()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    def stop():
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
    return opt[1], stop
