#!/usr/bin/env python3
"""codeeditor 的端到端验证（控制通道驱动）。

覆盖五层骨架、多标签、底部面板互切、编辑器编辑/撤销、查找替换、
搜索、菜单下拉、命令面板、分栏、主题——**声明式重写后这些链路必须等价**。

用法: python3 tools/codeeditor_e2e.py [binary] [ctl] [shots]
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "build", "dev", "bin", "codeeditor.exe")


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

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]

    def text(self, element_id):
        return self.ok("get", {"id": element_id})["props"].get("text", "")

    def click_at(self, x, y):
        self.ok("input.mouse", {"kind": "click", "x": x, "y": y, "button": 1})
        time.sleep(0.45)          # 等下一帧重组


def start(binary, shots):
    ctl = os.path.join(shots, "codeeditor-ctl.json")
    for path in (ctl,):
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
    log = open(os.path.join(shots, "codeeditor-e2e.log"), "wb")
    process = subprocess.Popen([binary, "--headless", "--control-port", "0",
                                "--control-file", ctl, "--shots", shots],
                               stdout=log, stderr=subprocess.STDOUT)
    for _ in range(100):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl, encoding="utf-8"))
                if info.get("port"):
                    return process, Client(info["port"], info.get("token", "")), info
            except Exception:
                pass
        time.sleep(0.2)
    raise RuntimeError("应用未在 20s 内写出控制文件")


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    shots = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "e2e-codeeditor")
    os.makedirs(shots, exist_ok=True)
    process, client, info = start(binary, shots)
    try:
        client.ok("hello")

        # —— 1. 五层骨架与兼容钩子 id ——
        for want in ("editor-page", "titlebar", "menubar", "activity-bar", "sidebar",
                     "editor-tabs", "editor", "bottom-panel", "bottom-tabs", "statusbar",
                     "status", "btn-theme", "language-label", "cursor-label", "sidebar-split"):
            check(client.count("#" + want) == 1, f"缺少元素 #{want}")
        print("[1] 五层骨架与兼容钩子 id 齐备")

        # —— 2. 初始标签与编辑器 ——
        check(client.ok("get", {"id": "editor"})["props"].get("language") == "cpp",
              "初始语言不是 cpp")
        check("renderer.cpp" in client.text("title-text"), "标题栏未显示初始文件")
        print("[2] 初始标签 renderer.cpp 已打开（语言 cpp）")

        # —— 3. 打开第二个文件（资源管理器按钮）——
        buttons = client.ok("find", {"selector": "Button"})["matches"]
        deploy = [b for b in buttons if "deploy.py" in json.dumps(b, ensure_ascii=False)]
        check(deploy, "资源管理器里没有 deploy.py")
        client.ok("invoke", {"id": deploy[0]["id"], "action": "click"})
        time.sleep(0.6)
        props = client.ok("get", {"id": "editor-tabs"})["props"]
        check("deploy.py" in props.get("options", ""), f"标签栏未加 deploy.py: {props}")
        check(client.ok("get", {"id": "editor"})["props"].get("language") == "python",
              "语言未跟随标签")
        print("[3] 打开 deploy.py → 标签 + 语言 + 编辑器内容联动")

        # —— 4. 底部面板三态互切（声明式分支切换：曾在此处段错误）——
        for index, want in ((0, "#problems-list"), (1, "#output-text"), (2, "#terminal-input")):
            client.ok("invoke", {"id": "bottom-tabs", "action": "select", "argument": str(index)})
            time.sleep(0.45)
            check(client.count(want) == 1, f"底部面板切到 {index} 后 {want} 不在")
        print("[4] 底部面板 问题/输出/终端 三态互切稳定")

        # —— 5. 编辑器：真实输入 + 撤销 ——
        client.ok("invoke", {"id": "editor", "action": "focus"})
        client.ok("input.text", {"id": "editor", "text": "ADDED_TOKEN"})
        check("ADDED_TOKEN" in client.text("editor"), "编辑器未收到输入")
        check(client.text("title-text").startswith("●"), "脏标记未出现在标题栏")
        client.ok("invoke", {"id": "editor", "action": "undo"})
        time.sleep(0.3)
        check("ADDED_TOKEN" not in client.text("editor"), "撤销未回退输入")
        print("[5] 编辑 → 脏标记 → 撤销 全链路正确")

        # —— 6. 搜索（真实输入 → 递归搜工作区/样例文本）——
        client.ok("invoke", {"id": "activity-search", "action": "click"})
        time.sleep(0.45)
        check(client.count("#search-input") == 1, "搜索面板未出现")
        client.ok("invoke", {"id": "search-input", "action": "focus"})
        client.ok("input.text", {"id": "search-input", "text": "raster"})
        time.sleep(0.7)
        check("命中" in client.text("status"), f"搜索未报命中: {client.text('status')}")
        print(f"[6] 搜索工作区 → {client.text('status')}")

        # —— 7. 菜单下拉 → 点条目执行命令 ——
        # 回归：`MenuBar` 曾把 `Click` 与 `MouseDown` 合在一个 case（两边都调
        # `on_open_menu`）——一次物理点击触发两次回调，"打开 → 又切回关闭"，
        # 菜单面板永远不出现。现在 MouseDown 只标命中、Click 才打开。
        client.ok("invoke", {"id": "activity-explorer", "action": "click"})
        time.sleep(0.4)
        client.click_at(16, 52)
        check(client.count("MenuPanel") == 1, "点击「文件」后菜单面板未出现")
        panel = client.ok("find", {"selector": "MenuPanel"})["matches"][0]
        check(panel["bounds"]["height"] > 0, "菜单面板高度为零")
        # 点第一个条目（「新建文件」）→ 打开快速打开面板 + 菜单关闭
        item_x = panel["bounds"]["x"] + 40
        item_y = panel["bounds"]["y"] + 16
        client.click_at(item_x, item_y)
        check(client.count("MenuPanel") == 0, "点了菜单条目后面板未关闭")
        check(client.count("#command-palette") == 1, "菜单条目未触发命令面板（快速打开）")
        print("[7] 菜单下拉 → 点条目执行命令（面板自动关闭）")
        # 关掉命令面板，恢复干净状态
        client.ok("invoke", {"id": "command-palette", "action": "close"})
        time.sleep(0.45)

        # —— 8. 分栏拖拽 + 主题 ——
        ratio0 = float(client.ok("get", {"id": "sidebar-split"})["props"]["ratio"])
        client.ok("invoke", {"id": "sidebar-split", "action": "step_forward"})
        time.sleep(0.4)
        ratio1 = float(client.ok("get", {"id": "sidebar-split"})["props"]["ratio"])
        check(ratio1 > ratio0, f"分栏比例未变化: {ratio0} → {ratio1}")
        client.ok("invoke", {"id": "btn-theme", "action": "click"})
        time.sleep(0.4)
        check(client.ok("get", {"id": "btn-theme"})["props"].get("label") in ("亮色", "暗色"),
              "主题按钮文案异常")
        print(f"[8] 分栏可拖（{ratio0:.2f} → {ratio1:.2f}）+ 主题切换")

        # —— 9. 截图（视觉核验素材）——
        shot = client.ok("capture", {"encode": "file",
                                     "path": os.path.join(shots, "codeeditor-declarative.png")})
        check(shot.get("path"), "截图未落盘")
        print(f"[9] 截图已落盘: {shot['path']}")

        print("\n[OK] codeeditor 端到端全部通过")
        return 0
    finally:
        try:
            client.ok("app", {"action": "quit"})
        except Exception:
            process.terminate()
        try:
            process.wait(timeout=5)
        except Exception:
            process.terminate()


if __name__ == "__main__":
    sys.exit(main())
