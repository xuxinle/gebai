#!/usr/bin/env python3
"""gbcode 的端到端验证（控制通道驱动）。

覆盖五层骨架、多标签、底部面板互切、编辑器编辑/撤销、查找替换、
搜索、菜单下拉、命令面板、分栏、主题——**声明式重写后这些链路必须等价**。

用法: python3 tools/gbcode_e2e.py [binary] [ctl] [shots]
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "build", "dev", "bin", "gbcode.exe")


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
    ctl = os.path.join(shots, "gbcode-ctl.json")
    for path in (ctl,):
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
    log = open(os.path.join(shots, "gbcode-e2e.log"), "wb")
    # `--tool-root` 指向框架仓根：终端测例要真跑 `st test`，
    # 而 PATH 里通常没有 `st`（工具链就在仓库的 build/bin 下）。
    framework_root = str(Path(__file__).resolve().parent.parent)
    process = subprocess.Popen([binary, "--headless", "--control-port", "0",
                                "--control-file", ctl, "--shots", shots,
                                "--tool-root", framework_root],
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
    shots = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "e2e-gbcode")
    os.makedirs(shots, exist_ok=True)
    process, client, info = start(binary, shots)
    try:
        client.ok("hello")

        # —— 1. 五层骨架与兼容钩子 id ——
        # 真终端形态（PTY 组件，无输入框）：终端钩子是 `#terminal` 与组件内
        # `#terminal-tabs`；`#terminal-input` 是假终端时代的钩子，已退役。
        for want in ("editor-page", "titlebar", "menubar", "activity-bar", "sidebar",
                     "editor-tabs", "editor", "editor-lower", "terminal", "terminal-tabs",
                     "statusbar",
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

        # —— 2b. 标题栏与菜单栏**合并成一行** ——
        #
        # 形态约束（产品决策：菜单在左、标题跟在其后）：菜单栏必须完整落在标题栏内，
        # 且标题栏只占一行——它不能再贡献第二个 32px 行。
        titlebar_box = client.ok("find", {"selector": "#titlebar"})["matches"][0]["bounds"]
        menubar_box = client.ok("find", {"selector": "#menubar"})["matches"][0]["bounds"]
        check(menubar_box["y"] >= titlebar_box["y"] - 1 and
              menubar_box["y"] + menubar_box["height"] <= titlebar_box["y"] + titlebar_box["height"] + 1,
              f"菜单栏不在标题栏内: 标题栏 {titlebar_box}, 菜单栏 {menubar_box}")
        check(menubar_box["x"] <= titlebar_box["x"] + 100,
              f"菜单栏不在标题栏左部: x={menubar_box['x']}")
        check(menubar_box["height"] >= titlebar_box["height"] - 1,
              f"菜单栏未占满行高: {menubar_box['height']} vs {titlebar_box['height']}")
        # 内容区必须紧跟这一行（合并不应该留下空心：`editor-page` 的 y == 标题栏底缘）
        page_box = client.ok("find", {"selector": "#editor-page"})["matches"][0]["bounds"]
        check(abs(page_box["y"] - (titlebar_box["y"] + titlebar_box["height"])) <= 1.0,
              f"内容区未紧跟合并后的标题行: page.y={page_box['y']}, 标题栏底={titlebar_box['y'] + titlebar_box['height']}")
        print(f"[2b] 标题栏与菜单栏合并成一行（{titlebar_box['height']:.0f}px 行内：菜单 {menubar_box['x']:.0f}→{menubar_box['x'] + menubar_box['width']:.0f}）")

        # —— 3. 打开第二个文件（资源管理器）——
        #
        # 资源管理器现在是 `Tree`（兼容钩子 `#workspace-tree` / 无工作区时的 `#sample-tree`），
        # 条目是**树行**而不是 Button——故改用树的行命中区点击，与“用户真去点那一行”一致。
        tree_sel = "#workspace-tree" if client.count("#workspace-tree") == 1 else "#sample-tree"
        tree_box = client.ok("find", {"selector": tree_sel})["matches"][0]["bounds"]
        # 第二行 = 第二个条目（无工作区时是 deploy.py）
        row_y = tree_box["y"] + 40 * 1.5
        client.click_at(tree_box["x"] + 60, row_y)
        time.sleep(0.6)
        props = client.ok("get", {"id": "editor-tabs"})["props"]
        check("deploy.py" in props.get("options", ""), f"标签栏未加 deploy.py: {props}")
        check(client.ok("get", {"id": "editor"})["props"].get("language") == "python",
              "语言未跟随标签")
        print("[3] 点资源管理器树行 → 标签 + 语言 + 编辑器内容联动")

        # —— 4. 终端面板（底部只有一个视图；问题/输出已删）——
        check(client.count("#problems-list") == 0, "问题面板应已删除")
        check(client.count("#output-text") == 0, "输出面板应已删除")
        check(client.count("#bottom-tabs") == 0, "底部标签栏应已删除（只剩终端）")
        # 真终端：PTY 属性为真、组件内标签栏就位、「+」在。
        check(client.ok("get", {"id": "terminal"})["props"].get("pty") == "true",
              "终端未进入 PTY 模式")
        tabs_props = client.ok("get", {"id": "terminal-tabs"})["props"]
        check(tabs_props.get("show_add") == "true", "终端标签栏未启用「+」新建")
        check("options" in tabs_props, "标签栏无会话数据")
        print("[4] 终端是真终端（PTY + 标签栏 + 「+」新建）")

        # —— 5. 编辑器：真实输入 + 撤销 ——
        client.ok("invoke", {"id": "editor", "action": "focus"})
        client.ok("input.text", {"id": "editor", "text": "ADDED_TOKEN"})
        check("ADDED_TOKEN" in client.text("editor"), "编辑器未收到输入")
        check(client.title("titlebar").startswith("●"), "脏标记未出现在标题栏")
        client.ok("invoke", {"id": "editor", "action": "undo"})
        time.sleep(0.3)
        check("ADDED_TOKEN" not in client.text("editor"), "撤销未回退输入")
        print("[5] 编辑 → 脏标记 → 撤销 全链路正确")

        # —— 6. 搜索（真实输入 → 回车 → 递归搜工作区/样例文本）——
        #
        # 搜索现在是“回车执行”（不再是每敲一键全量扫盘），故这里补一次 submit；
        # 断言也从状态栏改成**搜索面板自己的汇总**（状态栏文案会被其他动作盖掉）。
        client.ok("invoke", {"id": "activity-search", "action": "click"})
        time.sleep(0.45)
        check(client.count("#search-input") == 1, "搜索面板未出现")
        client.ok("invoke", {"id": "search-input", "action": "focus"})
        client.ok("input.text", {"id": "search-input", "text": "raster"})
        time.sleep(0.3)
        client.ok("invoke", {"id": "search-input", "action": "submit"})
        time.sleep(0.9)
        summary = client.text("search-summary")
        check("命中" in summary, f"搜索未报命中: {summary!r}")
        check(client.count("ListItem") > 0, "搜索有汇总但结果列表为空")
        print(f"[6] 搜索工作区 → {summary}（{client.count('ListItem')} 行结果）")

        # —— 7. 菜单下拉 → 点条目执行命令 ——
        # 回归：`MenuBar` 曾把 `Click` 与 `MouseDown` 合在一个 case（两边都调
        # `on_open_menu`）——一次物理点击触发两次回调，"打开 → 又切回关闭"，
        # 菜单面板永远不出现。现在 MouseDown 只标命中、Click 才打开。
        client.ok("invoke", {"id": "activity-explorer", "action": "click"})
        time.sleep(0.4)
        client.click_at(60, 20)
        check(client.count("MenuPanel") == 1, "点击「文件」后菜单面板未出现")
        panel = client.ok("find", {"selector": "MenuPanel"})["matches"][0]
        check(panel["bounds"]["height"] > 0, "菜单面板高度为零")
        # 点第一个条目（「新建文件」）→ 打开快速打开面板 + 菜单关闭
        item_x = panel["bounds"]["x"] + 40
        item_y = panel["bounds"]["y"] + 16
        client.click_at(item_x, item_y)
        check(client.count("MenuPanel") == 0, "点了菜单条目后面板未关闭")
        # 「新建文件」有工作区时开文件对话框、无工作区时新建内存缓冲——两者都是“开了个新编辑器”。
        check(client.count("#file-dialog") + client.count("#editor-tabs") > 0,
              "菜单条目未触发任何新建路径")
        print("[7] 菜单下拉 → 点条目执行命令（面板自动关闭）")
        # 关掉可能弹出的浮层，恢复干净状态（未开则跳过——“关不存在的面板”不该算失败）。
        if client.count("#command-palette") == 1:
            client.ok("invoke", {"id": "command-palette", "action": "close"})
            time.sleep(0.45)
        if client.count("#file-dialog") == 1:
            client.ok("invoke", {"id": "file-dialog", "action": "cancel"})
            time.sleep(0.45)

        # —— 8. 分栏拖拽 + 主题 ——
        ratio0 = float(client.ok("get", {"id": "sidebar-split"})["props"]["ratio"])
        client.ok("invoke", {"id": "sidebar-split", "action": "step_forward"})
        time.sleep(0.4)
        ratio1 = float(client.ok("get", {"id": "sidebar-split"})["props"]["ratio"])
        check(ratio1 > ratio0, f"分栏比例未变化: {ratio0} → {ratio1}")

        # 主题切换：**断言真值源与像素**，而不是按钮文案。
        #
        # 旧断言只看 `label in ("亮色","暗色")`——而缺陷恰恰是「文案翻转、画面不动」：
        # 页面维护了一个从不落地到主题的影子状态 `dark_`，于是这条断言**恒为真**，
        # 把一个完全不能用的功能报成通过（用户实测发现时就是这样）。
        mode_before = client.ok("theme", {})["mode"]
        pixels_before = client.ok("capture.hash", {})["hash"]
        client.ok("invoke", {"id": "btn-theme", "action": "click"})
        time.sleep(0.6)
        mode_after = client.ok("theme", {})["mode"]
        pixels_after = client.ok("capture.hash", {})["hash"]
        check(mode_after != mode_before,
              f"主题按钮未切换主题真值: {mode_before} → {mode_after}")
        check(pixels_after != pixels_before, "主题切换后画面没有变化")
        # 再切回，确认是**双向**可用（只能单向切也是坏的）
        client.ok("invoke", {"id": "btn-theme", "action": "click"})
        time.sleep(0.6)
        check(client.ok("theme", {})["mode"] == mode_before,
              f"主题切不回原档: 期望 {mode_before}，实际 {client.ok('theme', {})['mode']}")
        print(f"[8] 分栏可拖（{ratio0:.2f} → {ratio1:.2f}）+ 主题真值来回切换（{mode_before} ⇄ {mode_after}）")

        # —— 8b. 状态栏子元素占满行高 ——
        #
        # 回归：状态栏 `height=26` 但用了四边 `padding=10` → 子元素只剩 **6px** 高，
        # 图标与文字全被压扁，而容器自身尺寸「正确」（单看容器 bounds 看不出问题）。
        # 判据：固定高度的行里，子元素必须拿到几乎全部可用高度。
        bar = client.ok("find", {"selector": "#statusbar"})["matches"][0]["bounds"]
        button = client.ok("find", {"selector": "#btn-theme"})["matches"][0]["bounds"]
        check(button["height"] >= bar["height"] - 2,
              f"状态栏子元素被内边距压扁: 容器 {bar['height']:.0f}px，按钮 {button['height']:.0f}px")
        # 且按钮真的能被点到（压扁时 y 中心偏出自身范围）
        hit = client.ok("input.mouse", {"kind": "click",
                                        "x": button["x"] + button["width"] / 2,
                                        "y": button["y"] + button["height"] / 2,
                                        "button": 1})
        check(hit.get("hit", {}).get("id") == "btn-theme",
              f"状态栏按钮命中异常: {hit.get('hit', {}).get('id')}")
        # 上面那次真实点击会翻转主题：显式切回，别让后续步骤依赖隐式状态。
        time.sleep(0.5)
        if client.ok("theme", {})["mode"] != mode_before:
            client.ok("invoke", {"id": "btn-theme", "action": "click"})
            time.sleep(0.5)
        check(client.ok("theme", {})["mode"] == mode_before, "状态栏用例未能复原主题")
        print(f"[8b] 状态栏子元素占满行高（{bar['height']:.0f}px 容器 / {button['height']:.0f}px 按钮）且可点击")

        # —— 9. 截图（视觉核验素材）——
        shot = client.ok("capture", {"encode": "file",
                                     "path": os.path.join(shots, "gbcode-declarative.png")})
        check(shot.get("path"), "截图未落盘")
        print(f"[9] 截图已落盘: {shot['path']}")

        # —— 10. 菜单面板的**关闭路径**（本轮修复；旧行为：打开后关不掉）——
        # 根因：声明式 overlay 宿主是铺满视口的 `Panel`，命中按整块矩形算 → 吞掉全屏点击；
        # 而浮层键盘分派只问最外层宿主（它不认 Esc），真正的 `MenuPanel` 在子树里收不到。
        client.click_at(60, 20)
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
        client.click_at(60, 20)
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
        # 用**侧栏开合**触发一次重组（以前用底部标签切换）——不碰终端面板，
        # 否则后面的拖拽测例会因面板已收起而失去分栏（实测踩到）。
        client.ok("invoke", {"id": "btn-theme", "action": "click"})
        time.sleep(0.6)
        client.ok("invoke", {"id": "btn-theme", "action": "click"})
        time.sleep(0.6)
        props = client.ok("get", {"id": "editor"})["props"]
        check(props["language"] == "python", f"重组后语言被重置: {props['language']}")
        check(props["read_only"] == "true", f"重组后只读态被重置: {props['read_only']}")
        client.ok("set", {"id": "editor", "props": {"read_only": "false"}})
        print("[15] 属性面在重组后保持（language / read_only）")

        # —— 16. 页面撑满窗口（旧行为：页面只有 525px 高，底部 195px 空白）——
        #
        # 回归：页面根 `column` 漏写 `grow` 时，声明式布局只给它内容的自然高度，
        # 而根容器仍是全高——两个数的差值就是那截空白。断言用**比例**而不是绝对像素：
        # 窗口尺寸可配，而“根子元素应等于父内容槽”与尺寸无关。
        content_box = client.ok("find", {"selector": "#content"})["matches"][0]["bounds"]
        page_box = client.ok("find", {"selector": "#editor-page"})["matches"][0]["bounds"]
        check(abs(page_box["height"] - content_box["height"]) <= 1.0,
              f"页面未撑满内容槽: {page_box['height']:.0f} vs {content_box['height']:.0f}")
        # 编辑器应占编辑区的一半以上（否则“代码区太矮”还是没修）
        editor_box = client.ok("find", {"selector": "#editor"})["matches"][0]["bounds"]
        check(editor_box["height"] >= page_box["height"] * 0.35,
              f"编辑器高度不足（{editor_box['height']:.0f}px / 页面 {page_box['height']:.0f}px）")
        print(f"[16] 页面撑满内容槽（{page_box['height']:.0f}px；编辑器 {editor_box['height']:.0f}px）")

        # —— 17. 底部面板可拖（旧行为：固定 170px）——
        # 重新展开终端（前面的测例可能把它收起了；隐藏方案下用属性面判断）。
        if client.ok("get", {"id": "bottom-split"})["props"].get("second_hidden") == "true":
            client.ok("input.key", {"key": "j", "ctrl": True, "kind": "press"})
            time.sleep(0.6)
        split_box = client.ok("find", {"selector": "#bottom-split"})["matches"][0]["bounds"]
        upper_before = client.ok("find", {"selector": "#editor-upper"})["matches"][0]["bounds"]
        ratio = float(client.ok("get", {"id": "bottom-split"})["props"]["ratio"])
        handle_x = split_box["x"] + split_box["width"] * 0.5
        handle_y = split_box["y"] + (split_box["height"] - 8) * ratio + 4
        client.ok("input.mouse", {"kind": "down", "x": handle_x, "y": handle_y, "button": 1})
        client.ok("input.mouse", {"kind": "move", "x": handle_x, "y": handle_y - 120,
                                  "button": 1})
        client.ok("input.mouse", {"kind": "up", "x": handle_x, "y": handle_y - 120, "button": 1})
        time.sleep(0.5)
        upper_after = client.ok("find", {"selector": "#editor-upper"})["matches"][0]["bounds"]
        check(upper_after["height"] < upper_before["height"] - 40,
              f"拖分隔柄未改变上区高度: {upper_before['height']:.0f} → {upper_after['height']:.0f}")
        print(f"[17] 底部面板可拖（上区 {upper_before['height']:.0f} → {upper_after['height']:.0f}px）")

        # —— 18. 关闭脏标签会先确认（旧行为：直接丢修改）——
        #
        # 这是“可用”与“危险”的分界线：一个点一下就把用户未保存的修改抹掉的编辑器，
        # 不能算可用。三个按钮与 VSCode 同序（取消/不保存/保存）。
        # 入口：编辑器标签的 ×（工具栏 `#tool-close` 已随“头部只留 tab”移除）——
        # 走 `Tabs` 的 `close` 动作面（与点 × 同一条 `on_close` 链，不吃坐标猜测）。
        # 前置：[5] 的撤销把标签弄回了干净态——这里重新输入一点内容弄脏，
        # 否则关闭直接执行、弹不了确认（那不是缺陷，是"干净标签直接关"的正确行为）。
        client.ok("set", {"id": "editor", "props": {"read_only": "false"}})
        client.ok("invoke", {"id": "editor", "action": "focus"})
        client.ok("input.text", {"id": "editor", "text": "DIRTY_FOR_CLOSE"})
        time.sleep(0.4)
        check(client.title("titlebar").startswith("●"), "前置失败：输入后标签未变脏")
        client.ok("invoke", {"id": "editor-tabs", "action": "close"})
        time.sleep(0.6)
        check(client.count("#close-confirm-dialog") == 1, "关闭脏标签未弹确认对话框")
        actions = [b["text"] for b in client.ok("find", {"selector": "#close-confirm-dialog Button"})["matches"]]
        check(actions == ["取消", "不保存", "保存"], f"确认对话框按钮不符: {actions}")
        # 选“不保存”→标签真关闭
        tabs_before = client.ok("get", {"id": "editor-tabs"})["props"]["options"]
        client.ok("invoke", {"id": "close-confirm-dialog", "action": "invoke", "argument": "1"})
        time.sleep(0.6)
        check(client.count("#close-confirm-dialog") == 0, "选了“不保存”后对话框未关闭")
        tabs_after = client.ok("get", {"id": "editor-tabs"})["props"]["options"]
        check(tabs_after != tabs_before, "选了“不保存”后标签没关掉")
        print(f"[18] 关闭脏标签先确认（{tabs_before} → {tabs_after or '（无标签）'}）")

        # 终端长命令测例要 cd 到真仓根（`st test` 需要一个带 `st.pkg` 的目录）。
        root = Path(__file__).resolve().parent.parent

        # —— 19. 终端面板显隐（隐藏方案：状态保留）——
        #
        # 真终端 + 面板常驻隐藏：收起后 `#terminal` 子树**仍在树上**（只是不可见），
        # 会话与回看全部保留——这是本轮从"拆树重建"改过来的核心行为。
        # 判据：收起后 `second_hidden=true`、重开后 `false`，且 PTY 会话不掉。
        check(client.ok("get", {"id": "bottom-split"})["props"].get("second_hidden") == "false",
              "初始态底部面板应可见")
        # 面板头已并入组件标签栏：收起走组件动作面（与内置"收起"钮同一条链路）。
        client.ok("invoke", {"id": "terminal", "action": "collapse"})
        time.sleep(0.5)
        check(client.ok("get", {"id": "bottom-split"})["props"].get("second_hidden") == "true",
              "点收起后分栏未隐藏下半")
        check(client.ok("get", {"id": "terminal"})["props"].get("pty") == "true",
              "隐藏后终端会话不应丢失")
        client.ok("input.key", {"key": "j", "ctrl": True, "kind": "press"})
        time.sleep(0.6)
        check(client.ok("get", {"id": "bottom-split"})["props"].get("second_hidden") == "false",
              "Ctrl+J 未重新展开终端")

        # 真终端驱动：命令经 `send_line` 动作面送字节（shell 自己回显/执行）。
        def terminal(cmd, wait=1.0):
            client.ok("invoke", {"id": "terminal", "action": "send_line", "argument": cmd})
            time.sleep(wait)
            return client.ok("get", {"id": "terminal"})["props"].get("screen", "")

        # 会话里能真跑命令（pwd 回当前目录）。
        text = terminal("pwd")
        check("/" in text or "\\" in text, f"pwd 无路径输出: {text[-120:]}")
        print("[19] 终端面板显隐：隐藏保留会话 + 真跑 pwd")

        # —— 20. 终端长命令：中止（Ctrl+C）——
        #
        # 真终端里"中止"= `send_bytes("\x03")`（Ctrl+C）。跑一条 sleep，
        # 期间 shell 忙（提示符不回来），发 Ctrl+C 后提示符回归。
        client.ok("invoke", {"id": "terminal", "action": "send_line", "argument": "sleep 30"})
        time.sleep(0.8)
        # 中止动作面（面板按钮同一条路径）。
        client.ok("invoke", {"id": "terminal", "action": "stop"})
        time.sleep(0.8)
        # 提示符回归 = shell 活着且回到空闲（屏幕尾行是提示符而非空）。
        screen_after = client.ok("get", {"id": "terminal"})["props"].get("screen", "")
        tail = [line for line in screen_after.splitlines() if line.strip()]
        check(len(tail) > 0, "Ctrl+C 后屏幕为空")
        print("[20] 终端长命令：Ctrl+C 中止后 shell 回归空闲")

        # —— 21. 终端历史：↑ 翻出上一条 ——
        #
        # 真终端里历史归 shell（readline）：↑ 键翻译成 `\e[A` 字节送 PTY，
        # shell 回填到行内。驱动键事件 + 等屏幕出现上一条命令。
        client.ok("input.key", {"key": "ArrowUp", "kind": "press"})
        time.sleep(0.6)
        screen_hist = client.ok("get", {"id": "terminal"})["props"].get("screen", "")
        check("sleep 30" in screen_hist, f"↑ 未翻出上一条历史: {screen_hist[-120:]!r}")
        print("[21] 终端历史：↑ 翻出上一条（sleep 30）")

        # —— 22. 终端滚回：滚轮回看 + 滚动条 + 回到底部 ——
        #
        # 真终端的滚回在屏幕模型里（`scroll` 属性报 `偏移:总行数`）。
        # 产出一屏多的输出 → 滚轮上翻（偏移>0）→ 动作面回底（偏移归零）。
        client.ok("invoke", {"id": "terminal", "action": "send_line", "argument": "seq 1 60"})
        time.sleep(1.0)
        scroll_state = client.ok("get", {"id": "terminal"})["props"].get("scroll", "")
        total = int(scroll_state.split(":")[1]) if ":" in scroll_state else 0
        check(total > 20, f"长输出未产生滚回（total={total}）")
        # 滚轮上翻：终端矩形中心滚 5 格。
        term_box = client.ok("find", {"selector": "#terminal"})["matches"][0]["bounds"]
        cx = term_box["x"] + term_box["width"] / 2
        cy = term_box["y"] + term_box["height"] / 2
        for _ in range(5):
            client.ok("input.mouse", {"kind": "wheel", "x": cx, "y": cy, "delta": 1.0})
            time.sleep(0.15)
        scrolled = client.ok("get", {"id": "terminal"})["props"].get("scroll", "0:0")
        offset_now = int(scrolled.split(":")[0])
        check(offset_now > 0, f"滚轮未进入回看（offset={offset_now}）")
        # 滚动条可见且几何自洽（内容高 > 视口高）。
        bar = client.ok("get", {"id": "terminal"})["props"].get("scrollbar", "")
        check(bar != "", "回看时滚动条应可见")
        if bar:
            content_h, view_h, off = (float(v) for v in bar.split(":"))
            check(content_h > view_h, f"滚动条几何不自洽: {bar}")
        # 回到底部（动作面）。
        client.ok("invoke", {"id": "terminal", "action": "scroll_to_end"})
        time.sleep(0.4)
        back = client.ok("get", {"id": "terminal"})["props"].get("scroll", "0:0")
        check(int(back.split(":")[0]) == 0, f"回底部后偏移未归零: {back}")
        print(f"[22] 终端滚回：滚轮上翻（+{offset_now} 行）+ 滚动条 + 回到底部（滚回总量 {total}）")

        # —— 23. 菜单栏挂在标题栏里：**双击不得改窗口状态** ——
        #
        # 回归（本轮修，框架层）：菜单栏挂进标题栏的 `leading` 附属槽，而
        # `TitleBar::hits_caption` 那时只排除了尾部槽——于是双击「文件」菜单会
        # 冒泡到标题栏、落进“双击标题区 = 最大化/还原”。菜单自己消费了按下与单击，
        # 唯独双击漏出来（这就是“菜单栏按钮挂钩控制窗口事件”的现象）。
        # 判据用**属性面**（`maximized`）而不是像素：无头后端不支持窗口控制，
        # 属性会如实报 false；真正要钉住的是“动作有没有被触发”。
        before_title = client.title("titlebar")
        menu_box = client.ok("find", {"selector": "#menubar"})["matches"][0]["bounds"]
        # 前置：确保此刻没有已打开的下拉面板（否则下面那次单击会变成“关掉它”）
        client.ok("input.key", {"key": "Escape", "kind": "press"})
        time.sleep(0.5)
        check(client.count("MenuPanel") == 0, "前置失败：按下 Esc 后仍有下拉面板")
        client.ok("input.mouse", {"kind": "dblclick",
                                  "x": menu_box["x"] + 24, "y": menu_box["y"] + menu_box["height"] / 2,
                                  "button": 1})
        time.sleep(0.5)
        after_title = client.title("titlebar")
        check(before_title == after_title,
              f"双击菜单栏改动了标题/窗口状态: {before_title!r} -> {after_title!r}")
        check(client.count("MenuPanel") == 0, "双击不应把下拉面板打开/关掉")
        # 菜单本身仍要能点开（修的是“别触发窗口动作”，不是“别响应点击”）
        click_reply = client.ok("input.mouse", {"kind": "click", "x": menu_box["x"] + 24,
                                                "y": menu_box["y"] + menu_box["height"] / 2,
                                                "button": 1})
        time.sleep(0.5)
        check(client.count("MenuPanel") >= 1,
              f"单击菜单栏未打开下拉面板（命中 {click_reply.get('hit', {}).get('id')!r}）")
        # 再点**同一个标题** = 关闭（浏览器/VSCode 菜单栏的 toggle 手感）。
        # 这条同时钉住“点击不会漏到下层、也不会一次手势算两次”。
        client.ok("input.mouse", {"kind": "click", "x": menu_box["x"] + 24,
                                  "y": menu_box["y"] + menu_box["height"] / 2, "button": 1})
        time.sleep(0.45)
        check(client.count("MenuPanel") == 0, "再点已打开的菜单标题未关闭面板")
        print("[23b] 再点已打开的菜单标题 → 面板关闭（toggle 手感）")

        # —— 23c. 双击标题：面板**不能一闪就没**（真实双击序列）——
        # `LBUTTONDOWN, LBUTTONUP(+Click), LBUTTONDBLCLK, LBUTTONUP(+Click)`：
        # 后端曾在这里各补一个 Click，一次双击产生三个 Click，toggle 语义就“开了又关”。
        at = {"x": menu_box["x"] + 24, "y": menu_box["y"] + menu_box["height"] / 2}
        for kind in ("down", "up", "click"):
            client.ok("input.mouse", {"kind": kind, **at, "button": 1})
        time.sleep(0.35)
        check(client.count("MenuPanel") == 1, "第一次点击后面板未打开")
        for kind in ("down", "dblclick", "up"):
            client.ok("input.mouse", {"kind": kind, **at, "button": 1})
        time.sleep(0.45)
        check(client.count("MenuPanel") == 1, "双击菜单标题后面板消失了（一次手势被算成多次）")
        check(client.title("titlebar") == before_title, "双击菜单标题改动了窗口状态")
        print("[23c] 双击菜单标题 → 面板保持打开、窗口状态不变")
        client.ok("input.key", {"key": "Escape", "kind": "press"})
        time.sleep(0.4)
        print("[23] 菜单栏在标题栏内：双击不触发窗口动作、单击仍能开面板")

        # —— 24. Alt+↑/↓ 真移行（帮助卡声称的能力）——
        client.ok("invoke", {"id": "editor", "action": "focus"})
        client.ok("input.key", {"key": "a", "ctrl": True, "kind": "press"})
        client.ok("input.key", {"key": "Delete", "kind": "press"})   # 清空 → 三行确定性内容
        client.ok("input.text", {"id": "editor", "text": "aaa\nbbb\nccc"})
        time.sleep(0.4)
        client.ok("input.key", {"key": "Home", "ctrl": True, "kind": "press"})
        time.sleep(0.3)
        client.ok("input.key", {"key": "ArrowDown", "alt": True, "kind": "press"})
        time.sleep(0.5)
        moved = client.text("editor")
        check(moved.startswith("bbb") and "aaa" in moved.split("\n")[:2],
              f"Alt+↓ 未把首行下移: {moved.split(chr(10))[:3]}")
        client.ok("input.key", {"key": "ArrowUp", "alt": True, "kind": "press"})
        time.sleep(0.5)
        check(client.text("editor").startswith("aaa"), "Alt+↑ 未把行移回")
        print("[24] Alt+↑/↓ 真移行")

        # —— 25. Ctrl+D 选中下一处同词 ——
        client.ok("input.key", {"key": "Home", "ctrl": True, "kind": "press"})
        for _ in range(2):
            client.ok("input.key", {"key": "ArrowRight", "kind": "press"})
        time.sleep(0.3)
        client.ok("input.key", {"key": "d", "ctrl": True, "kind": "press"})
        time.sleep(0.5)
        selection = client.ok("get", {"id": "editor"})["props"].get("selection", "")
        picked = client.ok("get", {"id": "editor"})["props"].get("selected_text", "")
        check(picked == "aaa" and selection != "0:0",
              f"Ctrl+D 未选中同词: selection={selection} picked={picked!r}")
        print(f"[25] Ctrl+D 选中下一处同词（{picked!r}）")

        # —— 26. Ctrl+G 转到行：真浮层 + 真跳转 + 越界如实报错 ——
        client.ok("input.key", {"key": "g", "ctrl": True, "kind": "press"})
        time.sleep(0.6)
        check(client.count("#goto-input") == 1, "Ctrl+G 未打开转到行浮层")
        client.ok("input.text", {"id": "goto-input", "text": "2"})
        time.sleep(0.2)
        client.ok("input.key", {"key": "Enter", "kind": "press"})
        time.sleep(0.6)
        check(client.count("#goto-input") == 0, "提交后浮层未关闭")
        check(client.ok("get", {"id": "editor"})["props"].get("line") == "2",
              f"未跳到第 2 行: {client.ok('get', {'id': 'editor'})['props'].get('line')}")
        # 越界必须**留在浮层里报错**，而不是静默夹到末行
        client.ok("input.key", {"key": "g", "ctrl": True, "kind": "press"})
        time.sleep(0.5)
        client.ok("input.text", {"id": "goto-input", "text": "99999"})
        time.sleep(0.2)
        client.ok("input.key", {"key": "Enter", "kind": "press"})
        time.sleep(0.5)
        check(client.count("#goto-input") == 1 and client.count("#goto-error") == 1,
              "越界行号未在浮层内如实报错")
        check("超出范围" in client.text("goto-error"),
              f"越界提示文案不含“超出范围”: {client.text('goto-error')!r}")
        client.ok("input.key", {"key": "Escape", "kind": "press"})
        time.sleep(0.4)
        print("[26] Ctrl+G 转到行：真跳转 + 越界如实报错")

        # —— 27. 浮层里的 Enter 归输入框（回归：曾被子元素抢走）——
        #
        # 回归（本轮修，框架层）：`UiRoot::dispatch_key_into` 只逆序问所有子元素，
        # 于是浮层尾部的「×」按钮（`Button` 对 Enter 会 `activate()`）把输入框的
        # Enter 吃掉了——按回车等于把查找条关掉。现在浮层的按键先交给焦点元素。
        client.ok("input.key", {"key": "f", "ctrl": True, "kind": "press"})
        time.sleep(0.6)
        check(client.count("#find-bar") == 1, "Ctrl+F 未打开查找条")
        client.ok("input.text", {"id": "find-needle", "text": "aaa"})
        time.sleep(0.4)
        client.ok("input.key", {"key": "Enter", "kind": "press"})
        time.sleep(0.5)
        check(client.count("#find-bar") == 1, "查找条里的 Enter 被尾部按钮吃掉了（浮层被关）")
        check(client.text("find-counter") != "0/0", "查找未生效")
        client.ok("input.key", {"key": "Escape", "kind": "press"})
        time.sleep(0.4)
        check(client.count("#find-bar") == 0, "Esc 未关闭查找条")
        print("[27] 浮层内 Enter 归焦点输入框（不再被尾部按钮抢走）")

        # —— 27b. Ctrl+G 的浮层也要能被 Esc 关掉 ——
        # （只给查找条接 Esc 会让转到行变成“关不掉的浮层”，而它开着时快捷键又为它让路）
        client.ok("input.key", {"key": "g", "ctrl": True, "kind": "press"})
        time.sleep(0.5)
        check(client.count("#goto-input") == 1, "Ctrl+G 未打开转到行浮层")
        client.ok("input.key", {"key": "Escape", "kind": "press"})
        time.sleep(0.4)
        check(client.count("#goto-input") == 0, "Esc 未关闭转到行浮层")
        print("[27b] 转到行浮层可被 Esc 关闭")

        print("\n[OK] gbcode 端到端全部通过")
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
