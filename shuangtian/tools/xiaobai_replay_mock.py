"""把歌白的真实会话**回放**成 OpenAI 兼容的流式端点，供小白渲染检验。

## 为什么需要它

小白要验的是「思考 / 工具 / 正文」三种内容的**渲染质量**，而这三者是真实会话里
才会同时出现的东西。手工编几段假文本测不出问题（表格宽度、代码块换行、长思考的
折叠、工具参数折行——都要真实内容才露馅）。

所以：从 `users/<u>/sessions/**/chat.json` 挑几条真实会话，把它们**按 OpenAI
协议重放**：先推 `reasoning_content`（思考），再推 `tool_calls`（工具调用），
最后推 `content`（正文）。

## 用法

    py -3 tools/xiaobai_replay_mock.py --port 8770 --limit 6 [--session <chat.json>]

启动后把小白指向 `http://127.0.0.1:<port>/v1`，发一条消息即可看到一条
（按 `--turn` 选的）真实回复；每发一次消息换下一条，循环播放。
"""
import argparse
import glob
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# 回放顺序（每次请求取一条，用完循环）
_playlist = []
_cursor = [0]
_lock = threading.Lock()


def load_sessions(limit):
    """挑同时含「思考 / 工具 / 正文」的会话，并按内容规模排序取前 limit 条。"""
    # 会话不在框架仓库里，而在歌白的 `users/`（与 `shuangtian/` 同级）。
    here = os.path.dirname(os.path.abspath(__file__))          # .../shuangtian/tools
    repo = os.path.dirname(os.path.dirname(here))              # .../gebai-pro
    base = os.path.join(repo, "users")
    cands = []
    for path in glob.glob(os.path.join(base, "**", "chat.json"), recursive=True):
        try:
            data = json.load(open(path, encoding="utf-8"))
        except Exception:
            continue
        msgs = data.get("messages")
        if not isinstance(msgs, list):
            continue
        has_reason = any(m.get("reasoning") for m in msgs if isinstance(m, dict))
        has_tool = any(m.get("toolCalls") for m in msgs if isinstance(m, dict))
        has_text = any(isinstance(m, dict) and (m.get("content") or "").strip()
                       and not m.get("toolCalls") and m.get("role") == "assistant"
                       for m in msgs)
        if has_reason and has_tool and has_text:
            cands.append((len(msgs), data.get("name") or os.path.basename(os.path.dirname(path)), path, data))
    # 规模从大到小：内容多的会话更容易暴露排版问题
    cands.sort(key=lambda x: -x[0])
    return cands[:limit]


def build_turns(data):
    """把一个会话拆成若干「一轮回复」：每条 = {reasoning, tools[], content}。

    取法：按出现顺序，把相邻的 reasoning / toolCalls / content **归并到同一条
    assistant 消息**里——真实会话里它们往往在同一条上，也有一部分分散在多条。
    这里做的是「把同一轮的碎片拼好」，好让渲染侧一次看到完整的一轮。
    """
    msgs = data.get("messages") or []
    turns = []
    for m in msgs:
        if not isinstance(m, dict) or m.get("role") != "assistant":
            continue
        reasoning = (m.get("reasoning") or "").strip()
        content = (m.get("content") or "").strip()
        calls = m.get("toolCalls") or []
        if not reasoning and not content and not calls:
            continue
        tools = []
        for c in calls:
            name = c.get("name") or "tool"
            args = c.get("arguments")
            if not isinstance(args, str):
                args = json.dumps(args, ensure_ascii=False) if args is not None else "{}"
            tools.append((name, args))
        turns.append({"reasoning": reasoning, "tools": tools, "content": content})
    return turns


def next_turn():
    global _playlist
    with _lock:
        if not _playlist:
            return {"reasoning": "", "tools": [], "content": "（回放队列为空）"}
        idx = _cursor[0] % len(_playlist)
        _cursor[0] = idx + 1
        return _playlist[idx]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _send_json(self, status, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if "/models" in self.path:
            self._send_json(200, {"object": "list",
                                  "data": [{"id": "replay-mock", "object": "model"}]})
        else:
            self.send_error(404)

    def do_POST(self):
        if "/chat/completions" not in self.path:
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            request = json.loads(raw)
        except json.JSONDecodeError:
            self.send_error(400, "bad json")
            return
        if not isinstance(request, dict):
            self.send_error(400, "body must be object")
            return

        turn = next_turn()
        model = request.get("model", "replay-mock")

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()

        def chunk(delta, finish=None):
            payload = {"id": "replay", "object": "chat.completion.chunk",
                       "model": model,
                       "choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
            self.wfile.write(("data: " + json.dumps(payload, ensure_ascii=False) + "\n\n")
                             .encode("utf-8"))
            self.wfile.flush()

        # ① 思考：分段推（reasoning_content，DeepSeek/vLLM 口径）
        reasoning = turn["reasoning"]
        step = 48
        for i in range(0, len(reasoning), step):
            chunk({"reasoning_content": reasoning[i:i + step]})
            time.sleep(0.004)

        # ② 工具调用：先首片（带 id+name），再逐段推 arguments
        for ti, (name, args) in enumerate(turn["tools"]):
            chunk({"tool_calls": [{"index": ti, "id": f"call_{ti}",
                                   "type": "function",
                                   "function": {"name": name, "arguments": ""}}]})
            astep = 40
            for i in range(0, len(args), astep):
                chunk({"tool_calls": [{"index": ti,
                                       "function": {"arguments": args[i:i + astep]}}]})
                time.sleep(0.002)

        # ③ 正文：分段推
        content = turn["content"]
        for i in range(0, len(content), step):
            chunk({"content": content[i:i + step]})
            time.sleep(0.004)

        chunk({}, finish="tool_calls" if turn["tools"] else "stop")
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8770)
    ap.add_argument("--limit", type=int, default=6, help="扫多少个会话")
    ap.add_argument("--turns", type=int, default=6, help="回放多少轮（按内容量取前 N）")
    ap.add_argument("--session", default="", help="指定单个 chat.json")
    args = ap.parse_args()

    if args.session:
        data = json.load(open(args.session, encoding="utf-8"))
        sessions = [(0, data.get("name") or "session", args.session, data)]
    else:
        sessions = load_sessions(args.limit)

    if not sessions:
        print("没找到符合条件的会话（需同时含思考/工具/正文）")
        return 1

    turns = []
    for _, name, path, data in sessions:
        t = build_turns(data)
        print(f"  载入 {name[:36]:38} {len(t):4} 轮  {os.path.basename(os.path.dirname(path))[:8]}")
        turns.extend(t)

    # 按**类别均衡**挑选，而不是单纯按总字符量——
    #
    # 实测（2026-11-20）：真实会话里最长的那几轮几乎全是“思考几万字 + 正文几十字”，
    # 按总量排序会全选中它们，正文/表格/代码块的渲染根本没机会暴露。
    # 所以分四类各取几条：① 正文重（Markdown 表格/代码块）② 思考重 ③ 工具参数长
    # ④ 三者均衡（最能看出层与层之间的视觉分隔）。
    def w_text(t):
        return len(t["content"])

    def w_reason(t):
        return len(t["reasoning"])

    def w_tool(t):
        return sum(len(a) for _, a in t["tools"])

    def w_balance(t):
        # 均衡：取三者中的**最小值**——任一项为 0 就不算均衡
        return min(w_reason(t), w_text(t), w_tool(t)) if t["tools"] else 0

    pools = [
        ("正文重（表格/代码块）", w_text),
        ("思考重", w_reason),
        ("工具参数长", w_tool),
        ("三者均衡", w_balance),
    ]
    each = max(1, args.turns // len(pools))
    picks = []
    seen = set()
    for label, key in pools:
        ranked = sorted(turns, key=key, reverse=True)
        taken = 0
        for t in ranked:
            if taken >= each:
                break
            ident = id(t)
            if ident in seen:
                continue
            if key(t) <= 0:
                continue
            seen.add(ident)
            picks.append((label, t))
            taken += 1

    _playlist.extend(t for _, t in picks)
    print(f"\n共 {len(_playlist)} 轮待回放（从 {len(turns)} 轮里按类别挑）：")
    for i, (label, t) in enumerate(picks, 1):
        print(f"  [{i}] {label:18} 思考 {len(t['reasoning']):6} · 工具 {len(t['tools'])} "
              f"· 正文 {len(t['content']):5}")
    print(f"\n→ http://127.0.0.1:{args.port}/v1", flush=True)

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
