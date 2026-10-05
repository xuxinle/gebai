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

    def title(self, element_id):
        return self.ok("get", {"id": element_id})["props"].get("title", "")

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
        # 标题栏已换成自绘窗框组件（`ui::TitleBar`，id 仍为 `#titlebar`）。
        # ⚠ 读标题**必须查属性的 `title`，不能查 `text`**：`get` 的 `props` 里 `text`
        # 只在字段非空时写入，而组件声明的属性名是 `title`（`text` 是它的同义名——
        # `get_property` 认，但快照按 `property_names()` 逐个写）。
        # 旧写法查的是 `#title-text` 那个 Text 子元素——它已随窗框组件退役。
        check("renderer.cpp" in client.title("titlebar"), "标题栏未显示初始文件")
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
        check(client.title("titlebar").startswith("●"), "脏标记未出现在标题栏")
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

        # —— 10. 菜单面板的**关闭路径**（本轮修复；旧行为：打开后关不掉）——
        # 根因：声明式 overlay 宿主是铺满视口的 `Panel`，命中按整块矩形算 → 吞掉全屏点击；
        # 而浮层键盘分派只问最外层宿主（它不认 Esc），真正的 `MenuPanel` 在子树里收不到。
        client.click_at(16, 52)
        time.sleep(0.4)
        # 面板外的点击：瞬态浮层（菜单）应**关闭且不穿透**——
        # 旧行为：overlay 宿主是铺满视口的 Panel，把整屏点击都吃掉（什么都点不到）；
        # 中途尝试过“穿透”，但那会让“想关菜单”顺手触发背后的按钮（真实误操作）。
        check(client.count("MenuPanel") == 1, "重新打开菜单失败")
        outside = client.ok("input.mouse", {"kind": "click", "x": 1000, "y": 740, "button": 1})
        time.sleep(0.5)
        check(client.count("MenuPanel") == 0, "点面板外未关闭菜单面板")
        check(outside.get("hit", {}).get("id") != "content",
              "瞬态浮层应屏障面板外点击（不应穿透到下层内容）")
        # 关闭后重新打开必须还能开（反向验证“关闭”真回到了可交互状态）
        client.click_at(16, 52)
        time.sleep(0.4)
        check(client.count("MenuPanel") == 1, "关闭后无法重新打开菜单")
        client.ok("input.key", {"key": "Escape"})
        time.sleep(0.5)
        check(client.count("MenuPanel") == 0, "Esc 未关闭菜单面板")
        print("[10] 菜单面板 Esc / 面板外点击均可关闭（瞬态浮层屏障）")

        # —— 11. 命令面板：打开即聚焦 + 方向键导航 + Esc ——
        client.ok("input.key", {"key": "p", "ctrl": True, "shift": True})
        time.sleep(0.7)
        check(client.count("#command-palette") == 1, "命令面板未打开")
        focused = [m["id"] for m in client.ok("find", {"selector": ":focused"})["matches"]]
        check("palette-input" in focused,
              f"面板打开后焦点未交给过滤框（敲字会跑进代码里）: {focused}")
        active0 = int(client.ok("get", {"id": "command-palette"})["props"]["active"])
        client.ok("input.key", {"key": "ArrowDown"})
        time.sleep(0.35)
        active1 = int(client.ok("get", {"id": "command-palette"})["props"]["active"])
        check(active1 != active0, f"方向键未移动高亮（仍为 {active0}）")
        # 打字必须进面板（而不是底层编辑器）
        before_len = len(client.text("editor"))
        client.ok("input.text", {"text": "theme"})
        time.sleep(0.5)
        check(len(client.text("editor")) == before_len, "面板打开时输入跑进了编辑器")
        check(client.ok("get", {"id": "command-palette"})["props"]["query"] == "theme",
              "过滤词未进面板")
        client.ok("input.key", {"key": "Escape"})
        time.sleep(0.5)
        check(client.count("#command-palette") == 0, "Esc 未关闭命令面板")
        print("[11] 命令面板：打开即聚焦、方向键导航、Esc 可关")

        # —— 11b. 查找条是**非模态**浮层：开着也能点编辑器（与瞬态菜单相反）——
        client.ok("invoke", {"id": "editor", "action": "focus"})
        client.ok("input.key", {"key": "f", "ctrl": True})
        time.sleep(0.7)
        check(client.count("#find-needle") == 1, "查找条未出现")
        editor_box = client.ok("find", {"selector": "CodeEditor"})["matches"][0]["bounds"]
        hit = client.ok("input.mouse", {"kind": "click",
                                        "x": editor_box["x"] + 120,
                                        "y": editor_box["y"] + 120, "button": 1})
        check(hit.get("hit", {}).get("id") == "editor",
              f"非模态查找条吞掉了编辑器点击: {hit.get('hit')}")
        client.ok("invoke", {"id": "find-close", "action": "click"})
        time.sleep(0.4)
        print("[11b] 非模态浮层（查找条）不阻断下层交互")

        # —— 12. 编辑器翻页（旧行为：PageDown 把视口弹回顶部）——
        client.ok("set", {"id": "editor", "props": {
            "text": "\n".join(f"row {i}" for i in range(300))}})
        time.sleep(0.4)
        client.ok("invoke", {"id": "editor", "action": "focus"})
        line0 = int(client.ok("get", {"id": "editor"})["props"]["line"])
        client.ok("input.key", {"key": "PageDown"})
        time.sleep(0.45)
        line1 = int(client.ok("get", {"id": "editor"})["props"]["line"])
        check(line1 > line0, f"PageDown 未移动光标: {line0} → {line1}")
        client.ok("input.key", {"key": "PageUp"})
        time.sleep(0.45)
        line2 = int(client.ok("get", {"id": "editor"})["props"]["line"])
        check(line2 == line0, f"PageUp 未回到原行: {line0} → {line2}")
        print("[12] 编辑器翻页正确（PageDown {}→{}，PageUp 回到 {}）".format(line0, line1, line2))

        # —— 13. 拖垂直滚动条不改光标（旧行为：光标被拖到别的行）——
        editor_box = client.ok("find", {"selector": "CodeEditor"})["matches"][0]["bounds"]
        cursor_before = int(client.ok("get", {"id": "editor"})["props"]["cursor"])
        bar_x = editor_box["x"] + editor_box["width"] - 7
        client.ok("input.mouse", {"kind": "down", "x": bar_x,
                                  "y": editor_box["y"] + 20, "button": 1})
        client.ok("input.mouse", {"kind": "move", "x": bar_x,
                                  "y": editor_box["y"] + editor_box["height"] - 20, "button": 1})
        client.ok("input.mouse", {"kind": "up", "x": bar_x,
                                  "y": editor_box["y"] + editor_box["height"] - 20, "button": 1})
        time.sleep(0.45)
        props = client.ok("get", {"id": "editor"})["props"]
        check(int(props["cursor"]) == cursor_before,
              f"拖滚动条把光标挪走了: {cursor_before} → {props['cursor']}")
        check(float(props["scroll"].split(",")[1]) > 100.0, "拖滚动条未生效")
        print("[13] 垂直滚动条可拖且不动光标")

        # —— 14. 只读模式挡住所有编辑入口（旧行为：SelectAll+Delete 清空全文）——
        client.ok("set", {"id": "editor", "props": {"text": "keep me\nline two\n"}})
        time.sleep(0.3)
        client.ok("set", {"id": "editor", "props": {"read_only": "true"}})
        client.ok("invoke", {"id": "editor", "action": "select_all"})
        client.ok("input.key", {"key": "Delete"})
        time.sleep(0.35)
        client.ok("invoke", {"id": "editor", "action": "insert", "argument": "X"})
        time.sleep(0.35)
        text_now = client.text("editor")
        check(text_now == "keep me\nline two\n", f"只读模式被击穿: {text_now!r}")
        check(client.ok("get", {"id": "editor"})["props"]["read_only"] == "true",
              "只读标志自身被编辑重置了")
        client.ok("set", {"id": "editor", "props": {"read_only": "false"}})
        time.sleep(0.3)
        print("[14] 只读模式挡住 Delete/Backspace/insert")

        # —— 15. 属性面不被每次重组重置（旧行为：切底部面板 → language/read_only 回默认）——
        client.ok("set", {"id": "editor", "props": {"language": "python", "read_only": "true"}})
        time.sleep(0.3)
        client.ok("invoke", {"id": "bottom-tabs", "action": "select", "argument": "0"})
        time.sleep(0.7)
        props = client.ok("get", {"id": "editor"})["props"]
        check(props["language"] == "python", f"重组后语言被重置: {props['language']}")
        check(props["read_only"] == "true", f"重组后只读态被重置: {props['read_only']}")
        client.ok("set", {"id": "editor", "props": {"read_only": "false"}})
        print("[15] 属性面在重组后保持（language / read_only）")

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
