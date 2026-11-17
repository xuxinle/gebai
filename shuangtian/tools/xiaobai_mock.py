"""最小 OpenAI 兼容 /v1/chat/completions 流式 mock（仅标准库）。

用途：在没有真实 API Key 的环境里验证 st-agent 的流式链路
（HTTP POST → SSE 分块 → 增量解析 → 界面打字机追加）。

启动：py -3 tools/st_agent_mock.py [PORT]
默认 PORT=8765。请求体必须含 "stream": true；返回标准 SSE：
    data: {"choices":[{"delta":{"content":"…"}}]}
    ...
    data: [DONE]
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPLY = (
    "你好！我是本地 mock 端点返回的示例回复。\n\n"
    "这条回复是**流式**逐块推送的，用来验证：\n\n"
    "1. HTTP 请求正确发出（POST /v1/chat/completions）\n"
    "2. SSE 分块能被逐块解析\n"
    "3. 界面按增量打字机效果追加\n\n"
    "> 配置真实端点后即可换成真实模型。\n\n"
    "```python\nprint('streaming works')\n```\n"
)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # 静音默认访问日志
        pass

    def do_POST(self):
        if "/chat/completions" not in self.path:
            self.send_error(404, "not found")
            return
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            request = json.loads(raw)
        except json.JSONDecodeError:
            self.send_error(400, "bad json")
            return
        # 请求体必须是对象：收到数组/标量说明调用方把 JSON 拼错了
        # （单元素数组 `[{...}]` 是常见形态——初始化列表歧义）。
        if not isinstance(request, dict):
            self.send_error(400, "request body must be a JSON object")
            return

        if not request.get("stream", False):
            body = json.dumps(
                {
                    "id": "mock",
                    "object": "chat.completion",
                    "choices": [
                        {"index": 0, "message": {"role": "assistant", "content": REPLY}, "finish_reason": "stop"}
                    ],
                }
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        model = request.get("model", "mock-model")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()

        # 逐字符（小片）推送，模拟真实的 token 间隔
        step = 6
        for i in range(0, len(REPLY), step):
            piece = REPLY[i : i + step]
            payload = json.dumps(
                {
                    "id": "mock",
                    "object": "chat.completion.chunk",
                    "model": model,
                    "choices": [{"index": 0, "delta": {"content": piece}, "finish_reason": None}],
                },
                ensure_ascii=False,
            )
            self.wfile.write(f"data: {payload}\n\n".encode())
            self.wfile.flush()
            time.sleep(0.02)
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"mock OpenAI endpoint: http://127.0.0.1:{port}/v1/chat/completions")
    sys.stdout.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
