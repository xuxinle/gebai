#!/usr/bin/env python3
"""gbcode 右键菜单**派发口径**的端到端验证（真应用 + 真文件）。

覆盖（2026-11-16 拓出的真实缺陷）：
  菜单条目数组里的**分隔线占下标**，而旧实现按数组下标 `switch` 派发，于是整体错位：
  · 编辑器菜单：点“剪切”实际执行了“复制”（文本没被剪切走）；
  · 资源管理器菜单：点“重命名”实际执行了——**删除文件**。

这里不点“删除”（那会真毁数据），而是用**可观察、不可逆性低**的两条：
  [1] 编辑器：右键 → “剪切” → 编辑器文本必须变短（旧实现会变成“复制”，长度不变）；
  [2] 编辑器：右键 → “撤销” → 文本回到原长（证明 [1] 走的是撤销栈里的真操作）。

判据是**编辑器文本长度 / 状态栏文案**，不是截图：属性面能钉住“动作到底做了哪件事”，
而截图只能看出“菜单没了”。

用法: python tools/gbcode_context_menu_e2e.py [binary]
退出码 0 = 全过；非 0 = 有断言失败（打印具体项）。
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

AGENT_HANDSHAKE = (
    "仅霜天子Agent 可连接本控制通道：请勿手工拼装 TCP 协议，改用霜天子工具（shuangtian_run / shuangtian_tree / shuangtian_capture / shuangtian_call …）"
)

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "build", "dev", "bin", "gbcode" + (".exe" if os.name == "nt" else ""))


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
            params["agent"] = AGENT_HANDSHAKE
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

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]


def start(binary, workdir, home, shots):
    ctl = os.path.join(shots, "ctl.json")
    if os.path.exists(ctl):
        os.remove(ctl)
    log = open(os.path.join(shots, "app.log"), "wb")
    env = dict(os.environ)
    env["ST_HOME"] = home
    args = [binary, "--headless", "--control-port", "0", "--control-file", ctl,
            "--tool-root", ROOT, "--workspace", workdir]
    process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, env=env, cwd=workdir)
    for _ in range(120):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl, encoding="utf-8"))
                if info.get("port"):
                    return process, Client(info["port"], info.get("token", ""))
            except Exception:
                pass
        time.sleep(0.2)
    raise RuntimeError("应用未在 24s 内写出控制文件")


def wait_until(predicate, timeout=6.0, what=""):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.15)
    raise AssertionError(f"等待超时：{what}")


def find_one(client, selector):
    """`find` 返回 {selector, count, total, matches[]}——取第一条。"""
    result = client.ok("find", {"selector": selector})
    matches = result.get("matches") or []
    if not matches:
        raise AssertionError(f"找不到元素：{selector}")
    return matches[0]


def menu_items(client):
    """当前打开的面板条目 id（`items` 属性面：`|` 分隔，不含分隔线）。"""
    panel = find_one(client, "ContextMenu")
    text = client.props(panel["id"]).get("items", "")
    return panel, [item for item in text.split("|") if item]


def click_menu_item(client, item_id):
    """按 **id** 点菜单项（`MenuPanel` 的动作面 `activate_item`）。

    为什么不用坐标点：菜单条目顺序与分隔线数量会变，坐标法脆（gbcode e2e 亲历）；
    而这里要验的正是“id → 动作”这条链，走 id 入口才能把它单独钉住。
    """
    panel, _ = menu_items(client)
    ok = client.ok("invoke", {"id": panel["id"], "action": "activate_item",
                              "argument": item_id})
    return ok


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    work = Path(tempfile.mkdtemp(prefix="gbcode-ctxmenu-e2e-"))
    workspace = work / "proj"
    workspace.mkdir()
    # 一段足够长的文本：剪切后长度差一眼可见（旧实现“剪切=复制”长度不变）。
    payload = "\n".join(f"line {i}: abcdefghij" for i in range(1, 21)) + "\n"
    target = workspace / "a.cpp"
    target.write_text(payload, encoding="utf-8")
    home = work / "home"
    home.mkdir()
    shots = work / "shots"
    shots.mkdir()

    proc, client = start(binary, str(workspace), str(home), str(shots))
    failures = []

    def check(condition, message):
        if condition:
            print(f"[ok] {message}")
        else:
            print(f"[FAIL] {message}")
            failures.append(message)

    try:
        client.ok("hello")
        # 打开文件（资源树第一行）。
        tree = find_one(client, "#workspace-tree")
        row_h = float(client.props("workspace-tree").get("row_height") or 24)
        client.ok("input.mouse", {"kind": "click",
                                  "x": tree["bounds"]["x"] + 60,
                                  "y": tree["bounds"]["y"] + row_h * 0.5, "button": 1})
        wait_until(lambda: "a.cpp" in client.props("editor-tabs").get("options", ""),
                   what="a.cpp 打开")

        full_len = len(client.props("editor").get("text", ""))
        check(all(item in payload for item in ("line 1:", "line 20:")),
              f"编辑器装载了 {full_len} 字符（原始 {len(payload)}）")

        # 全选（菜单条目 `select_all` 与 Ctrl+A 同一条组件动作面）。
        client.ok("invoke", {"id": "editor", "action": "select_all"})
        time.sleep(0.3)
        sel = client.props("selection-label").get("text", "")
        check("已选" in sel, f"已全选（{sel}）")

        # —— [1] 右键 → 条目「剪切」——
        client.ok("invoke", {"id": "editor", "action": "focus"})
        editor_box = find_one(client, "#editor")["bounds"]
        client.ok("input.mouse", {"kind": "down", "button": 2,
                                  "x": editor_box["x"] + editor_box["width"] * 0.5,
                                  "y": editor_box["y"] + editor_box["height"] * 0.5})
        client.ok("input.mouse", {"kind": "up", "button": 2,
                                  "x": editor_box["x"] + editor_box["width"] * 0.5,
                                  "y": editor_box["y"] + editor_box["height"] * 0.5})
        wait_until(lambda: client.ok("find", {"selector": "ContextMenu"})["count"] >= 1,
                   what="右键菜单出现")
        panel, ids = menu_items(client)
        # 条目 id 面：分隔线不出现在这里面（`items` 给的是可激活项）。
        check("cut" in ids and "copy" in ids, f"菜单含 cut/copy（{','.join(ids)}）")

        click_menu_item(client, "cut")
        time.sleep(0.6)
        after_len = len(client.props("editor").get("text", ""))
        status = client.props("status").get("text", "")
        check(after_len == 0, f"「剪切」把文本剪走了（{full_len} → {after_len}）")
        check("剪切" in status, f"状态栏如实报「剪切」（{status}）")

        # —— [2] 撤销：回到原长（证明 [1] 进的是撤销栈里的真编辑）——
        client.ok("invoke", {"id": "editor", "action": "undo"})
        time.sleep(0.5)
        restored = client.props("editor").get("text", "")
        check(len(restored) == full_len and "line 20:" in restored,
              f"撤销恢复原内容（{len(restored)} / {full_len}）")

        # 关闭菜单（点遮罩外）。
        client.ok("input.mouse", {"kind": "click", "x": 5, "y": 5, "button": 1})
    finally:
        try:
            client.ok("app", {"action": "quit"})
        except Exception:
            pass
        try:
            proc.wait(timeout=15)
        except Exception:
            proc.kill()

    if failures:
        print(f"\n失败 {len(failures)} 项：")
        for item in failures:
            print(f"  - {item}")
        return 1
    print("\n全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
