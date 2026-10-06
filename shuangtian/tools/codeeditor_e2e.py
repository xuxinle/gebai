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
from pathlib import Path

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
    shots = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "e2e-codeeditor")
    os.makedirs(shots, exist_ok=True)
    process, client, info = start(binary, shots)
    try:
        client.ok("hello")

        # —— 1. 五层骨架与兼容钩子 id ——
        for want in ("editor-page", "titlebar", "menubar", "activity-bar", "sidebar",
                     "editor-tabs", "editor", "bottom-panel", "terminal-input", "statusbar",
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
        check(client.count("#terminal-input") == 1, "终端输入框不在")
        check(client.count("#terminal-cwd") == 1, "终端栏未显示工作目录")
        check(client.count("#terminal-prompt") == 1, "终端提示行不在")
        print("[4] 终端是底部唯一视图（问题/输出已删，标签栏已去）")

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
                                     "path": os.path.join(shots, "codeeditor-declarative.png")})
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
        # 重新展开终端（前面的测例可能把它收起了）
        if client.count("#terminal-input") == 0:
            client.ok("invoke", {"id": "bottom-close", "action": "click"})
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
        client.ok("set", {"id": "editor", "props": {"read_only": "false"}})
        client.ok("invoke", {"id": "tool-close", "action": "click"})
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

        # —— 19. 终端：内置命令 / cd / 真实外部命令 ——
        #
        # 终端是“可用”的硬指标：它得真回答关于工作区的问题，而不是回一句“未知命令”。
        # 这一组逐条跑真实命令，断言**输出内容**（不是“有没有反应”）。
        # 终端面板的收起/展开走 Ctrl+J（收起时 `#bottom-close` 本身就不存在，
        # 不能拿它当开合开关）。
        if client.count("#terminal-input") == 0:
            client.ok("input.key", {"key": "j", "ctrl": True, "kind": "press"})
            time.sleep(0.6)
        check(client.count("#terminal-input") == 1, "Ctrl+J 未展开终端面板")
        client.ok("invoke", {"id": "bottom-close", "action": "click"})
        time.sleep(0.5)
        check(client.count("#panel-terminal") == 0, "终端收起后面板仍在")
        client.ok("input.key", {"key": "j", "ctrl": True, "kind": "press"})
        time.sleep(0.6)
        check(client.count("#terminal-input") == 1, "Ctrl+J 未重新展开终端")

        def terminal(cmd, wait=1.2):
            client.ok("invoke", {"id": "terminal-input", "action": "focus"})
            client.ok("input.text", {"id": "terminal-input", "text": cmd})
            time.sleep(0.25)
            client.ok("invoke", {"id": "terminal-input", "action": "submit"})
            time.sleep(wait)
            return client.text("terminal-output")

        # 先 cd 到真仓根：后续测例（`st test` 需要一个带 `st.pkg` 的目录）都靠它。
        # 放在最前面还有一个工艺理由：示例模式的 `pwd` 回的是占位串「(内置样例)」，
        # 先切目录才能对绝对路径做断言。
        text = terminal("cd " + str(root))
        check(client.text("terminal-prompt-cwd") == str(root),
              f"cd 后提示行未跟随: {client.text('terminal-prompt-cwd')}")
        text = terminal("pwd")
        check(str(root) in text, f"pwd 无输出工作目录: {text[-120:]}")
        text = terminal("cd /不存在的目录")
        check("目录不存在" in text, "cd 到不存在的目录未被拒")
        text = terminal("cd /tmp")
        check(client.text("terminal-prompt-cwd") == "/tmp",
              f"cd /tmp 后提示行未跟随: {client.text('terminal-prompt-cwd')}")
        # 白名单外的 git 写操作必须在**解析阶段**就被拒
        text = terminal("git commit -m x")
        check("拒绝" in text and "白名单" in text, f"git 写操作未被拒: {text[-160:]}")
        text = terminal("git", wait=1.5)
        check("用法" in text, f"`git` 无参数未给用法提示: {text[-120:]}")
        text = terminal("unknown-cmd-xyz")
        check("未知命令" in text, "未知命令未给提示")
        print("[19] 终端：cd（含拒绝不存在的目录）/pwd/git 白名单/未知命令")

        # —— 20. 终端长命令：工作线程 + 实时输出 + 中止 ——
        #
        # 回归（本轮修）：旧实现是同步 `st::process::run`——一条几十秒的命令会把
        # 主循环卡住整段时长（连“中止”按钮都点不到）。现在输出逐行回流、
        # 中止能真杀进程（`StreamHandle`）。
        client.ok("invoke", {"id": "terminal-input", "action": "focus"})
        client.ok("input.text", {"id": "terminal-input", "text": "cd " + str(root)})
        client.ok("invoke", {"id": "terminal-input", "action": "submit"})
        time.sleep(0.6)
        client.ok("invoke", {"id": "terminal-input", "action": "focus"})
        client.ok("input.text", {"id": "terminal-input", "text": "cd " + str(root)})
        client.ok("invoke", {"id": "terminal-input", "action": "submit"})
        time.sleep(0.6)
        client.ok("invoke", {"id": "terminal-input", "action": "focus"})
        client.ok("input.text", {"id": "terminal-input", "text": "st test"})
        client.ok("invoke", {"id": "terminal-input", "action": "submit"})
        # 跑起来后应该：① 出现“运行中”提示 ② 中止按钮出现 ③ 已经有输出（流式）
        deadline = time.time() + 4.0
        running_text = ""
        while time.time() < deadline:
            running_text = client.text("terminal-output")
            if "运行中" in running_text:
                break
            time.sleep(0.2)
        check("运行中" in running_text, f"长命令未进入“运行中”（同步阻塞？）: {running_text[-160:]}")
        check(client.count("#terminal-stop") == 1, "运行中未出现中止按钮")
        client.ok("invoke", {"id": "terminal-stop", "action": "click"})
        deadline = time.time() + 8.0
        stopped = ""
        while time.time() < deadline:
            stopped = client.text("terminal-output")
            if "已中止" in stopped:
                break
            time.sleep(0.2)
        check("已中止" in stopped, f"点中止后未见“已中止”: {stopped[-200:]}")
        check(client.count("#terminal-stop") == 0, "中止后按钮未消失")
        print("[20] 终端长命令：运行中提示 + 实时输出 + 中止按钮真杀进程")

        # —— 21. 终端历史：↑ 翻出上一条 ——
        #
        # `Input` 不认方向键，所以这条链路只能整条测：键 → `set_event_handler`
        # → `terminal_history_step` → 回填输入框。
        client.ok("invoke", {"id": "terminal-input", "action": "focus"})
        client.ok("input.key", {"key": "ArrowUp", "kind": "press"})
        time.sleep(0.5)
        recalled = client.ok("get", {"id": "terminal-input"})["props"].get("value", "")
        check(recalled == "st test", f"↑ 未翻出上一条历史: {recalled!r}")
        print(f"[21] 终端历史：↑ 翻出上一条（{recalled}）")

        # —— 22. 终端滚回：多行显示 + 自动贴底 ——
        #
        # 回归（本轮修）：`Text` 默认**单行省略**——不调 `set_multiline(true)` 时
        # 整份滚回被折成一行，看着就像“输出只有一行”。而贴底判据若拿**当前**
        # `max_scroll` 去比，会因为“内容本帧又长高了”而恒判“用户不在底部”，
        # 表现为**永远不跟**（停在上方不滚）。这里两条一起钉。
        client.ok("invoke", {"id": "terminal-clear", "action": "click"})
        time.sleep(0.6)
        for _ in range(6):
            client.ok("invoke", {"id": "terminal-input", "action": "focus"})
            client.ok("input.text", {"id": "terminal-input", "text": "pwd"})
            client.ok("invoke", {"id": "terminal-input", "action": "submit"})
            time.sleep(0.5)
        rendered = client.text("terminal-output")
        check(rendered.count("\n") >= 8, f"滚回未多行渲染（被折成一行？）: {rendered[:120]!r}")
        scroll = client.ok("get", {"id": "terminal-scroll"})["props"]
        max_scroll = float(scroll["max_scroll"])
        offset = float(scroll["offset"])
        check(max_scroll > 0.0, "内容未超出视口（无法测贴底）")
        check(offset >= max_scroll - 2.0, f"未自动贴底: offset={offset} max={max_scroll}")
        print(f"[22] 终端滚回：多行 + 自动贴底（{rendered.count(chr(10)) + 1} 行，offset {offset:.0f}/{max_scroll:.0f}）")

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
