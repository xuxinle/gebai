#!/usr/bin/env python3
"""gbcode 会话恢复的端到端验证（控制通道驱动）。

覆盖：最近打开的 MRU 记录 → 关窗（app quit）保存 → 重启恢复
（工作区/标签/活动项/光标）→ 菜单与命令面板的「最近」入口。

用法: python3 tools/gbcode_e2e_session.py [binary] [workdir]
"""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# 非 UTF-8 控制台自愈：GBK 代码页下 print 中文/符号（如 '⇄'）会 UnicodeEncodeError 崩整跑；
# 已是 UTF-8 则不动，残余不可编字符以 '?' 兜底（宁可错印不崩）。
if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "build", "dev", "bin", "gbcode" + (".exe" if os.name == "nt" else ""))
PY = sys.executable


class Client:
    def __init__(self, port, token):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.rid = 0
        self.token = token

    def call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        header = b""
        while len(header) < 4:
            chunk = self.sock.recv(4 - len(header))
            if not chunk:
                raise RuntimeError("控制通道在收到响应头前关闭")
            header += chunk
        length = struct.unpack(">I", header)[0]
        payload = b""
        while len(payload) < length:
            chunk = self.sock.recv(length - len(payload))
            if not chunk:
                raise RuntimeError("控制通道在收到完整响应前关闭")
            payload += chunk
        return json.loads(payload)

    def ok(self, method, params=None):
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def text(self, element_id):
        return self.ok("get", {"id": element_id})["props"].get("text", "")

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]

    def title(self, element_id):
        return self.ok("get", {"id": element_id})["props"].get("title", "")

    def click_at(self, x, y):
        self.ok("input.mouse", {"kind": "click", "x": x, "y": y, "button": 1})
        time.sleep(0.45)


def check(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  PASS {message}" if False else f"[ok] {message}")


def start(binary, workdir, home, shots, extra_args=()):
    """启动 gbcode（ST_HOME 重定向到临时目录，会话文件不碰真实用户数据）。"""
    ctl = os.path.join(shots, "ctl.json")
    if os.path.exists(ctl):
        os.remove(ctl)
    log = open(os.path.join(shots, "app.log"), "wb")
    env = dict(os.environ)
    env["ST_HOME"] = home   # default_session_path 的重定向口
    args = [binary, "--headless", "--control-port", "0", "--control-file", ctl,
            "--shots", shots, "--tool-root", ROOT, *extra_args]
    process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, env=env,
                               cwd=workdir)
    for _ in range(120):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl, encoding="utf-8"))
                if info.get("port"):
                    return process, Client(info["port"], info.get("token", "")), info
            except Exception:
                pass
        time.sleep(0.2)
    raise RuntimeError("应用未在 24s 内写出控制文件")


def quit_and_wait(process, client):
    client.ok("app", {"action": "quit"})
    process.wait(timeout=15)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    work = Path(tempfile.mkdtemp(prefix="gbcode-session-e2e-"))
    workspace = work / "proj"
    workspace.mkdir()
    # 工作区文件：a.cpp 有 40 行（光标要跳到中后部才有区分度）。
    (workspace / "a.cpp").write_text(
        "\n".join(f"line {i} of a" for i in range(1, 41)) + "\n", encoding="utf-8")
    (workspace / "b.py").write_text(
        "\n".join(f"# line {i} of b" for i in range(1, 11)) + "\n", encoding="utf-8")
    home = work / "home"          # ST_HOME（会话文件在这里）
    home.mkdir()
    shots = work / "shots"
    shots.mkdir()
    session_json = home / ".shuangtian" / "gbcode" / "session.json"

    # —— 第一轮：打开两个文件、跳行、退出 ——
    # `--open` 直接给 a.cpp（它走 `open_path`，MRU 埋点同一条路）；b.py 点资源树。
    proc, client, _ = start(binary, str(work), str(home), str(shots),
                            ["--workspace", str(workspace), "--open", str(workspace / "a.cpp")])
    try:
        client.ok("hello")
        check("a.cpp" in client.ok("get", {"id": "editor-tabs"})["props"].get("options", ""),
              "--open 已打开 a.cpp")
        # b.py：点资源树。工作区树**没有根行**（直接从条目开始，字典序：
        # a.cpp 第 1 行、b.py 第 2 行）。行高从树属性读（不硬编码）。
        tree = client.ok("find", {"selector": "#workspace-tree"})["matches"]
        check(len(tree) == 1, "工作区树已出现")
        tree_box = tree[0]["bounds"]
        row_h = float(client.ok("get", {"id": "workspace-tree"})["props"].get("row_height") or 26)
        rows = int(client.ok("get", {"id": "workspace-tree"})["props"].get("rows") or 0)
        check(rows >= 2, f"树有 {rows} 行（两个文件）")
        client.click_at(tree_box["x"] + 60, tree_box["y"] + row_h * 1.5)   # 第 2 行 = b.py
        tabs = client.ok("get", {"id": "editor-tabs"})["props"].get("options", "")
        check("b.py" in tabs, f"标签栏出现 b.py: {tabs}")
        # 活动标签切回 a.cpp：Ctrl+Tab 环绕。
        # ⚠ 2026-10-18：标签集只有 a.cpp / b.py 两个了（不再有启动时自动打开的
        # 内置样例 renderer.cpp），所以当前 active=b.py 时**按一次**就回到 a.cpp——
        # 旧断言写死“连按两次”是因为有三个标签，会给多按一次就跳过头。
        client.ok("invoke", {"id": "editor", "action": "focus"})
        client.ok("input.key", {"key": "Tab", "ctrl": True, "kind": "press"})
        time.sleep(0.4)
        active_label = client.title("titlebar")
        check("a.cpp" in active_label, f"Ctrl+Tab 切回 a.cpp: {active_label!r}")
        # 光标到第 30 行（区分度：恢复后应停在 30 附近）。
        client.ok("set", {"id": "editor", "props": {"line": "30"}})
        time.sleep(0.4)
        line_now = int(client.ok("get", {"id": "editor"})["props"]["line"])
        check(line_now >= 29, f"光标已到第 {line_now} 行（目标 30）")
    finally:
        quit_and_wait(proc, client)
    print("[1] 第一轮：两文件打开、活动 a.cpp、光标 30 行 → 退出")

    # —— 会话文件校验 ——
    check(session_json.exists(), f"会话文件已落盘: {session_json}")
    doc = json.loads(session_json.read_text(encoding="utf-8"))
    check(doc.get("workspace", "").endswith("proj"), f"工作区已存: {doc.get('workspace')!r}")
    paths = [f["path"].replace("\\", "/") for f in doc.get("files", [])]
    check(any(p.endswith("a.cpp") for p in paths), f"a.cpp 在会话: {paths}")
    check(any(p.endswith("b.py") for p in paths), f"b.py 在会话: {paths}")
    a_entry = next(f for f in doc.get("files", []) if f["path"].replace("\\", "/").endswith("a.cpp"))
    check(a_entry.get("line", 0) >= 29, f"光标行已存: {a_entry.get('line')}")
    check(any(p.replace("\\", "/").endswith("a.cpp") for p in doc.get("recent_files", [])),
          f"recent_files 含 a.cpp: {doc.get('recent_files')}")
    check(any(p.replace("\\", "/").lower().endswith("proj") for p in doc.get("recent_folders", [])),
          f"recent_folders 含工作区: {doc.get('recent_folders')}")
    print(f"[2] session.json 字段齐（{len(paths)} 标签，recent {len(doc.get('recent_files', []))} 条）")

    # —— 第二轮：无 --workspace/--open 启动 → 断言恢复 ——
    proc, client, _ = start(binary, str(work), str(home), str(shots))
    try:
        client.ok("hello")
        # 工作区恢复：树从「内置样例」变「工作区树」。
        check(client.count("#workspace-tree") == 1, "工作区已恢复（资源树为工作区树）")
        tabs = client.ok("get", {"id": "editor-tabs"})["props"].get("options", "")
        check("a.cpp" in tabs and "b.py" in tabs, f"标签已恢复: {tabs}")
        # 活动标签 + 光标。
        title = client.title("titlebar")
        check("a.cpp" in title, f"活动标签恢复为 a.cpp: {title!r}")
        line_now = int(client.ok("get", {"id": "editor"})["props"]["line"])
        check(line_now >= 29, f"光标恢复到第 {line_now} 行（会话存 {a_entry.get('line')}）")
        # 打开的是 a.cpp 的内容（不是残留的样例）。
        editor_text = client.text("editor")
        check("line 30 of a" in editor_text, "编辑器内容是 a.cpp（非样例残留）")
        # 未标脏（打开/恢复不冒充编辑——上一轮修复的回归）。
        check(not title.startswith("●"), "恢复后未标脏")

        # —— 菜单「最近打开」 ——
        client.click_at(60, 20)   # 「文件」菜单
        time.sleep(0.4)
        check(client.count("MenuPanel") == 1, "文件菜单已打开")
        panel = client.ok("find", {"selector": "MenuPanel"})["matches"][0]
        box = panel["bounds"]
        # 静态区 10 项（new…close-all）+ 分隔 + 最近文件夹 1 + 分隔 + 最近文件 2 = 16 行。
        # 点「最近文件」区第二项 = b.py（MRU：a.cpp 刚被打开过应在最前……实际顺序按
        # 最近打开时间：a.cpp 后被打开（跳转后仍算最近），所以第一项是 a.cpp。
        # 点第一项最近文件（a.cpp）——已在标签里则激活，不断言新标签。
        # 更稳的目标：最近文件夹条目（打开文件夹：<proj 名>）——点它切换工作区
        # 到同一路径（无变化），能证明分发链路通。
        items_total = int(round(box["height"] / 32))
        check(items_total >= 14, f"菜单含最近小节（共 {items_total} 行）")
        # 最近文件夹 = 分隔(11) 后第 12 行（0 起 11）。
        client.click_at(box["x"] + 40, box["y"] + 16 + 11 * 32)
        time.sleep(0.5)
        check(client.count("MenuPanel") == 0, "点最近文件夹后面板关闭（分发链路通）")

        # —— 命令面板「最近」条目 ——
        client.ok("input.key", {"key": "p", "ctrl": True, "shift": True})
        time.sleep(0.6)
        check(client.count("#command-palette") == 1, "命令面板已打开")
        client.ok("invoke", {"id": "command-palette", "action": "focus"})
        # 过滤「最近」：面板条目过滤按 title/detail 匹配。
        client.ok("input.text", {"id": "palette-input", "text": "最近"})
        time.sleep(0.5)
        entries = client.ok("find", {"selector": "#command-palette ListItem"})["matches"]
        check(len(entries) >= 1, f"命令面板过滤「最近」命中 {len(entries)} 条")
        client.ok("input.key", {"key": "Escape"})
        time.sleep(0.4)
    finally:
        quit_and_wait(proc, client)
    print("[3] 第二轮：无参数启动 → 工作区/标签/活动/光标全部恢复；菜单与命令面板「最近」入口可用")

    print("\n[OK] gbcode 会话恢复端到端全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
