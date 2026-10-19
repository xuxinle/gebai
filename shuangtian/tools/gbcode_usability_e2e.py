#!/usr/bin/env python3
"""gbcode 深度用户体验走查（"真正可用"验收）：控制通道驱动、真工作区、带断言。

为什么单列一个脚本（与 `gbcode_e2e.py` 的分工）：
- `gbcode_e2e.py` 走**功能面**（29 组：骨架/标签/终端/查找/面板/主题…），
  用的是内置样例工作区；
- 本脚本走**真实使用者的路径**（真磁盘工作区 + 真 git 仓 + 真 shell）：
  文件读写保真、编码/BOM、编辑手感、终端忙闲语义、菜单/Git 写操作、
  以及"Agent 能不能零坐标驱动"（树行/菜单项的可寻址性）。

两者都不覆盖对方：功能面全绿时用户照样会在"打开带 BOM 的文件""按 Shift+Tab"
"看运行面板永远转圈"上翻车——那正是本脚本钉住的部分（2026-10-18 实测所得）。

用法: python3 tools/gbcode_usability_e2e.py [二进制] [产物目录]
退出码 0 = 全部通过。
"""
from __future__ import annotations

import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BIN = ROOT / "build" / "dev" / "bin" / ("gbcode.exe" if os.name == "nt" else "gbcode")

failures: list[str] = []


def check(condition: bool, message: str) -> bool:
    print(("  OK   " if condition else "  FAIL ") + message, flush=True)
    if not condition:
        failures.append(message)
    return condition


class Client:
    def __init__(self, port: int, token: str) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=60.0)
        self.sock.settimeout(60.0)
        self.rid = 0
        self.token = token

    def call(self, method: str, params: dict | None = None) -> dict:
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        header = b""
        while len(header) < 4:
            chunk = self.sock.recv(4 - len(header))
            if not chunk:
                raise RuntimeError("控制通道提前关闭")
            header += chunk
        length = struct.unpack(">I", header)[0]
        payload = b""
        while len(payload) < length:
            chunk = self.sock.recv(length - len(payload))
            if not chunk:
                raise RuntimeError("控制通道提前关闭")
            payload += chunk
        return json.loads(payload)

    def ok(self, method: str, params: dict | None = None) -> dict:
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    # —— 便捷读写 ——
    def get(self, element_id: str, props: dict | None = None) -> dict:
        params = {"id": element_id}
        if props:
            params["props"] = props
        return self.ok("get", params)["props"]

    def text(self, element_id: str) -> str:
        return self.get(element_id).get("text", "")

    def count(self, selector: str) -> int:
        return self.ok("find", {"selector": selector})["count"]

    def bounds(self, selector: str) -> dict:
        return self.ok("find", {"selector": selector})["matches"][0]["bounds"]

    def key(self, name: str, ctrl: bool = False, shift: bool = False, alt: bool = False) -> dict:
        params = {"kind": "press", "key": name}
        if ctrl:
            params["ctrl"] = True
        if shift:
            params["shift"] = True
        if alt:
            params["alt"] = True
        return self.ok("input.key", params)

    def tap(self, name: str) -> dict:
        """逐字符键盘（走 KeyDown + TextInput，与真键盘同路径）。"""
        return self.ok("input.key", {"kind": "press", "key": name, "text": name})

    def type_text(self, text: str, element_id: str | None = None) -> dict:
        params = {"text": text}
        if element_id:
            params["id"] = element_id
        return self.ok("input.text", params)

    def click_at(self, x: float, y: float) -> None:
        self.ok("input.mouse", {"kind": "click", "x": x, "y": y, "button": 1})
        time.sleep(0.4)

    def click_id(self, element_id: str) -> dict:
        reply = self.ok("input.mouse", {"kind": "click", "id": element_id, "button": 1})
        time.sleep(0.4)
        return reply

    def invoke(self, element_id: str, action: str, argument: str | None = None) -> dict:
        params = {"id": element_id, "action": action}
        if argument is not None:
            params["arguments"] = argument
            params["argument"] = argument
        return self.ok("invoke", params)

    def set_props(self, element_id: str, props: dict) -> dict:
        return self.ok("set", {"id": element_id, "props": props})


def start(binary: Path, workspace: Path, shots: Path) -> tuple[subprocess.Popen, Client]:
    shots.mkdir(parents=True, exist_ok=True)
    ctl = shots / "ctl.json"
    try:
        ctl.unlink()
    except FileNotFoundError:
        pass
    args = [str(binary), "--headless", "--control-port", "0", "--control-file", str(ctl),
            "--shots", str(shots), "--workspace", str(workspace)]
    env = dict(os.environ)
    # ST_HOME 重定向：会话恢复会让上一轮的工作区/标签污染本轮（见 gbcode_e2e 同注）。
    env["ST_HOME"] = str(shots)
    log = open(shots / "app.log", "wb")
    process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, env=env)
    for _ in range(240):
        if ctl.exists():
            try:
                info = json.load(open(ctl, encoding="utf-8"))
                if info.get("port"):
                    client = Client(info["port"], info.get("token", ""))
                    client.ok("hello")
                    return process, client
            except Exception:
                pass
        time.sleep(0.15)
    raise RuntimeError("应用未在 36s 内写出控制文件")


def make_workspace(root: Path) -> None:
    if root.exists():
        shutil.rmtree(root, ignore_errors=True)
    (root / "src").mkdir(parents=True)
    (root / "docs").mkdir()
    (root / "src" / "main.py").write_text(
        "def hello():\n    print('hi')\n\n# TODO: fix me\nreturn 1\n", encoding="utf-8")
    (root / "src" / "util.py").write_text("VALUE = 42\n", encoding="utf-8")
    (root / "docs" / "中文 空格.md").write_text("# 中文标题\n\n正文一段。\n", encoding="utf-8")
    (root / "README.md").write_text("# workspace\n\ntypo here\n", encoding="utf-8")
    (root / "big.txt").write_text("\n".join(f"line {i}" for i in range(1, 2001)), encoding="utf-8")
    (root / "binary.bin").write_bytes(bytes(range(256)) * 32)
    # 带 UTF-8 BOM 的文件（Windows 上极常见：记事本 / 旧版 VS）
    (root / "bom.txt").write_bytes(b"\xef\xbb\xbfhello\n")
    # CRLF 行尾（要能原样保真写回）
    (root / "crlf.txt").write_bytes(b"alpha\r\nbeta\r\ngamma\r\n")


def open_quick(client: Client, query: str, settle: float = 1.1) -> str:
    """Ctrl+P 快速打开（返回编辑器文本）。"""
    client.key("p", ctrl=True)
    time.sleep(0.5)
    client.type_text(query)
    time.sleep(0.7)
    tabs = client.ok("get", {"id": "command-palette"})
    matches = (tabs.get("props") or {}).get("match_count", "?")
    client.key("Enter")
    time.sleep(settle)
    return str(matches)


def main() -> int:
    binary = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BIN
    shots = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "build" / "e2e-gbcode-usability"
    workspace = Path(tempfile.gettempdir()) / "gbcode-usability-ws"
    if not binary.exists():
        print(f"未找到 {binary}（先 st build gbcode）")
        return 2
    make_workspace(workspace)
    print(f"工作区: {workspace}")
    process, client = start(binary, workspace, shots)
    try:
        client.invoke("activity-explorer", "click")
        time.sleep(0.8)

        # —— 1. 资源树：行内容可读 + 零坐标驱动 ——
        # Tree 是自绘组件（行不是 Element）。没有 `labels`/`rows_detail`，
        # 调用方（人和 Agent）都只能靠截图猜"树里有什么"；没有 `activate_key`，
        # 就只能靠"bounds + 行高 × 行号"算坐标——而行号会随展开/收起漂移。
        if not client.count("#workspace-tree"):
            client.invoke("activity-search", "click")
            time.sleep(0.6)
            client.invoke("activity-explorer", "click")
            time.sleep(0.8)
        tree = client.get("workspace-tree")
        labels = (tree.get("labels") or "").split("\n")
        keys = (tree.get("keys") or "").split("\n")
        check(len(labels) == int(tree.get("rows", "0")), "树行标签数与 rows 一致")
        check("README.md" in labels and "src" in labels, f"树里能读到真实条目（{labels[:4]}…）")
        detail = tree.get("rows_detail") or ""
        check(detail.count("\n") + 1 == len(labels) and "true" in detail,
              "rows_detail 给出每行的 (label,key,is_dir,depth,expanded)")

        readme_key = next((k for n, k in zip(labels, keys) if n == "README.md"), "")
        reply = client.set_props("workspace-tree", {"activate_key": readme_key})
        time.sleep(1.1)
        check("activate_key" in (reply.get("changed") or []), "按 key 激活被受理")
        check("README.md" in (client.get("editor-tabs").get("options") or ""),
              "按 key 激活 → 文件真打开（零坐标依赖）")
        check("typo here" in client.text("editor"), "打开的确实是那个文件的内容")

        before_rows = int(client.get("workspace-tree").get("rows"))
        src_key = next((k for n, k in zip(labels, keys) if n == "src"), "")
        client.set_props("workspace-tree", {"activate_key": src_key})
        time.sleep(1.2)
        after_tree = client.get("workspace-tree")
        check(int(after_tree.get("rows")) > before_rows, "按 key 激活目录 → 展开出子行")
        check("main.py" in (after_tree.get("labels") or ""), "展开后能读到子文件名")
        bad = client.set_props("workspace-tree", {"activate_key": "dir:没有这个目录"})
        check(any(item.get("name") == "activate_key" for item in (bad.get("rejected") or [])),
              "不存在的 key 被如实拒绝（不静默成功）")

        # —— 2. 文本保真：BOM / CRLF / 中文名 ——
        open_quick(client, "bom.txt")
        text = client.text("editor")
        check(not text.startswith("\ufeff"), "带 BOM 的文件打开后 BOM 已剥离")
        check(text.startswith("hello"), f"BOM 文件正文正确（{text[:12]!r}）")

        open_quick(client, "crlf.txt")
        client.invoke("editor", "focus")
        client.type_text("X", "editor")
        time.sleep(0.3)
        client.key("s", ctrl=True)
        time.sleep(0.9)
        raw = (workspace / "crlf.txt").read_bytes()
        check(raw.startswith(b"Xalpha") and raw.count(b"\r\n") >= 3, "CRLF 行尾原样保留（保存不改成 LF）")

        open_quick(client, "中文")
        check("# 中文标题" in client.text("editor"), "中文/空格文件名可打开，内容不串码")

        # —— 3. 编辑手感：脏标记、保存、撤销、Shift+Tab ——
        open_quick(client, "main.py")
        client.invoke("editor", "focus")
        client.type_text("EDITED_TOKEN\n", "editor")
        time.sleep(0.4)
        check(client.get("titlebar").get("title", "").startswith("●"), "编辑后标题栏出现脏标记")
        client.key("s", ctrl=True)
        time.sleep(0.9)
        check("EDITED_TOKEN" in (workspace / "src" / "main.py").read_text(encoding="utf-8"),
              "Ctrl+S 真写盘")
        check(not client.get("titlebar").get("title", "").startswith("●"), "保存后脏标记清空")
        client.invoke("editor", "undo")
        time.sleep(0.4)
        check("EDITED_TOKEN" not in client.text("editor"), "撤销回退输入")
        client.invoke("editor", "redo")
        time.sleep(0.4)
        check("EDITED_TOKEN" in client.text("editor"), "重做恢复输入")

        # 无选区 Shift+Tab：必须是**反缩进**（旧实现反而增加缩进——键坏了级别的手感缺陷）
        (workspace / "src" / "indent.py").write_text("    indented = 1\nplain = 2\n", encoding="utf-8")
        open_quick(client, "indent.py")
        client.set_props("editor", {"cursor": "6", "selection": "6:6"})
        time.sleep(0.3)
        client.invoke("editor", "focus")
        client.key("Tab", shift=True)
        time.sleep(0.5)
        first_line = client.text("editor").split("\n")[0]
        check(not first_line.startswith("    "), f"无选区 Shift+Tab 反缩进（首行 {first_line!r}）")

        # 自动配对（逐字符键盘路径）
        (workspace / "src" / "pair.py").write_text("x = 1\n", encoding="utf-8")
        open_quick(client, "pair.py")
        # cursor=5 就是「x = 1」的行尾（换行符之前）——那里才是配对的触发点。
        client.set_props("editor", {"cursor": "5", "selection": "5:5"})
        time.sleep(0.3)
        client.invoke("editor", "focus")
        client.tap("(")
        time.sleep(0.4)
        check("()" in client.text("editor"), f"行尾输入 ( 自动配对为 ()（{client.text('editor')[:12]!r}）")

        # —— 4. 查找替换（真实 id）——
        client.key("f", ctrl=True)
        time.sleep(0.5)
        check(client.count("#find-bar") == 1, "Ctrl+F 打开查找条")
        client.invoke("find-needle", "focus")
        client.type_text("x", "find-needle")
        time.sleep(0.7)
        counter = client.text("find-counter")
        check("/" in counter, f"查找计数可读（{counter}）")
        client.invoke("find-replace", "focus")
        client.type_text("y", "find-replace")
        client.invoke("find-replace-all", "click")
        time.sleep(0.6)
        check("x = 1" not in client.text("editor") or "y" in client.text("editor"),
              "全部替换真的改了正文")
        client.key("Escape")
        time.sleep(0.3)

        # —— 5. 终端：忙闲语义（旧实现恒真）+ 标签短名 ——
        terminal = client.get("terminal")
        check(terminal.get("busy") == "false", "刚启动的终端不忙（旧实现恒为 true）")
        check(terminal.get("running") == "true", "但 shell 进程是活着的（running）")
        tabs_options = client.get("terminal-tabs").get("options", "")
        check("\\" not in tabs_options and "/" not in tabs_options,
              f"终端标签是短名而不是完整路径（{tabs_options}）")
        client.invoke("terminal", "send_line", "ping -n 4 127.0.0.1" if os.name == "nt"
                      else "sleep 2")
        time.sleep(1.0)
        check(client.get("terminal").get("busy") == "true", "命令运行中 busy=true")
        back = False
        for _ in range(24):
            time.sleep(0.5)
            if client.get("terminal").get("busy") == "false":
                back = True
                break
        check(back, "命令跑完后 busy 回 false（运行面板不再永远转圈）")

        # —— 6. 菜单项：零坐标激活（不是靠"面板高 / 条目数"算坐标）——
        client.click_at(60, 20)   # 点开「文件」
        time.sleep(0.5)
        panel = client.ok("find", {"selector": "MenuPanel"})
        check(panel["count"] == 1, "点标题栏打开「文件」菜单")
        if panel["count"] == 1:
            panel_id = panel["matches"][0]["id"]
            check("另存为" in (panel["matches"][0].get("text") or ""), "菜单条文本可读")
            reply = client.invoke(panel_id, "activate_item", "open")
            time.sleep(1.0)
            check(client.count("FileDialog") == 1, "按 id 激活「打开文件」→ 真的弹出文件对话框")
            client.key("Escape")
            time.sleep(0.4)

        # —— 7. 状态栏每个格子都有反应（不能“点了什么也不发生”）——
        #
        # ⚠ 不能用单一的“状态栏文本变没变”做判据：那只是其中一类的**副作用**。
        # 三类格子的“反应”各不相同：
        #   · `status-indent` / `status-encoding`：点一下把说明写进状态栏文本（文本变）；
        #   · `language-label`：点一下切到侧栏视图（状态栏文本**本就不该变**，
        #     它显示的是当前语言）；
        #   · `status-blame`：纯图标激活按钮（`text` 恒为空），反应是 `variant` 变 `soft`。
        # 旧版对四个格子一律比“status 文本”，后面两个注定为假——不是产品缺陷，是判据错位。
        for element_id, hint in (("status-indent", "缩进宽度"), ("status-encoding", "编码说明")):
            before = client.text("status")
            client.invoke(element_id, "click")
            time.sleep(0.6)
            after = client.text("status")
            check(before != after, f"状态栏「{hint}」点击有可见反馈")

        # 语言格：反应是**侧栏切到导航结果视图**（`SidebarView::Nav`），不是状态栏文本。
        client.invoke("language-label", "click")
        time.sleep(0.8)
        check(client.count("#sidebar-nav") == 1,
              "状态栏「语言」点击 → 侧栏切到语言/导航视图（有可见反馈）")
        client.invoke("activity-explorer", "click")
        time.sleep(0.5)

        # blame 格：反应是 `variant` 从 ghost → soft（纯图标激活按钮的语义）。
        # 注意：`find` 的匹配项不带 `props`，读属性必须走 `get`。
        def blame_variant():
            return (client.ok("get", {"id": "status-blame"}).get("props") or {}).get("variant")

        blame_before = blame_variant()
        client.invoke("status-blame", "click")
        time.sleep(0.6)
        blame_after = blame_variant()
        check(blame_before != blame_after,
              f"状态栏「blame 开关」点击有可见反馈（variant {blame_before!r} → {blame_after!r}）")
        # 切回去，不影响后续用例的初始态。
        client.invoke("status-blame", "click")
        time.sleep(0.4)

        # —— 8. Git：真仓库的写操作链路 ——
        #
        # ⚠ 仓库必须**在启动前**就是 git 仓（或者切走再切回 SCM 视图）：
        # `activity-scm` 是 toggle，而启动时默认已停在「资源管理器」侧栏视图，
        # 点一次正好切到 SCM（这份测试已依赖这个前提）；但如果先前已在 SCM 视图，
        # 再点一次会**收起侧栏**（与 VSCode 同行为）——所有 scm-* id 就集体消失。
        # 所以下面每次要用 SCM 都先确认元素在不在，不在就“切别人再切回来”。
        def git(*args: str) -> str:
            return subprocess.run(["git", *args], cwd=workspace, capture_output=True,
                                  text=True, encoding="utf-8", errors="replace").stdout

        def open_scm_panel() -> None:
            for _ in range(3):
                if client.count("#scm-toolbar") or client.count("[id=scm-refresh-toolbar]"):
                    return
                client.invoke("activity-explorer", "click")
                time.sleep(0.7)
                client.invoke("activity-scm", "click")
                time.sleep(1.5)

        git("init", "-q", "-b", "main")
        git("config", "user.email", "e2e@local")
        git("config", "user.name", "E2E")
        git("add", ".")
        git("commit", "-qm", "first")
        (workspace / "README.md").write_text("# workspace\n\ntypo here\nchanged\n", encoding="utf-8")
        open_scm_panel()
        check(client.count("#git-file-README.md") == 1,
              "变更面板列出被修改的真文件（README.md 行可见）")
        if client.count("#git-file-README.md"):
            client.invoke("git-file-README.md", "click")
            time.sleep(1.5)
            check(client.count("DiffView") == 1, "点变更条目 → 打开差异视图")
            client.key("w", ctrl=True)
            time.sleep(0.5)
        if client.count("#stage-README.md"):
            client.invoke("stage-README.md", "click")
            time.sleep(1.5)
            status = git("status", "--porcelain")
            check(any(line[:2].strip() in ("M", "A") and "README.md" in line
                      for line in status.splitlines()),
                  "暂存按钮真的把文件加进暂存区")
            check(client.count("#git-group-已暂存") == 1, "面板出现「已暂存」分组（刷新真的落地）")
            if client.count("#commit-message"):
                client.invoke("commit-message", "focus")
                client.type_text("usability e2e commit", "commit-message")
                time.sleep(0.3)
                client.invoke("commit-go", "click")
                time.sleep(2.5)
                check("usability e2e commit" in git("log", "--oneline", "-3"),
                      "提交真的落到 git 历史")

        # —— 9. 大文件：可用而不卡死 + **编辑帧不随行数增长** ——
        started = time.time()
        open_quick(client, "big.txt", settle=2.0)
        elapsed = time.time() - started
        lines = client.get("editor").get("lines")
        check(elapsed < 10.0, f"2000 行文件打开耗时 {elapsed:.1f}s（< 10s）")
        check(lines and int(lines) >= 2000, f"行数正确（{lines}）")
        box = client.bounds("#editor")
        for _ in range(5):
            client.ok("input.mouse", {"kind": "wheel", "x": box["x"] + box["width"] / 2,
                                      "y": box["y"] + box["height"] / 2, "delta": -1.0})
            time.sleep(0.1)
        check(client.get("editor").get("scroll") != "0.0,0.0", "滚轮能滚动大文件")

        # —— 9b. 大文档编辑帧的性能上界（回归：“整页变卡”）——
        #
        # 缺陷形态（实测）：5 千行文档里每敲一个字符，单帧 paint 从 4.5ms 涨到
        # 90ms——因为文本宽度缓存在超 4096 条时**全清**，而文档行数恰好在这个量级，
        # 于是每次编辑都在“全清 → 全部重新整形”的抖动区。
        # 用户感知就是“整页都卡了、打字有延迟”。
        #
        # 这里用**真文档规模**（≈5 千行、真实的 CJK + markdown 长行）量编辑帧，
        # 断言 paint 回到帧预算内（< 40ms；修复前实测 90ms，修复后 ≈ 13ms）。
        big_doc = workspace / "perf_doc.md"
        lines_seed = [f"## 小节 {i}：这是用于性能回归的一段说明文字，长度接近真实文档" for i in range(5000)]
        big_doc.write_text("\n".join(lines_seed), encoding="utf-8")
        open_quick(client, "perf_doc.md", settle=2.5)
        check(int(client.get("editor").get("lines") or 0) >= 5000,
              "性能样本是 5 千行级文档")
        client.invoke("editor", "focus")
        time.sleep(0.4)
        client.type_text("x", "editor")
        time.sleep(0.6)
        metrics = client.ok("metrics")
        paint_ms = float(metrics.get("paint_ms") or 0.0)
        check(paint_ms < 40.0,
              f"5 千行文档编辑帧 paint {paint_ms:.1f}ms（< 40ms；旧实现实测 ~90ms）")

        # —— 9c. Markdown 预览：滚过大代码块后内容必须仍然完整 ——
        #
        # 缺陷形态（2026-10-18 用户报「预览滚一点就大片内容没渲染」）：
        # 组背景（`code_bg` 大面板）与文字曾在**同一个逐行循环**里画。
        # 代码块首行滚出视口后，旧代码对**每一行**都重画整块背景——
        # 于是行 2 的背景把行 1 的字盖掉，依次类推，屏幕上只剩最后一行那点字。
        # 真机截图：一个 30 行的 ASCII 图代码块只剩底部一条框线，内部全空。
        #
        # 为何必须在 e2e 里守：单元测试的 `FixedAdvanceTextPort` **不真的光栅化**，
        # 像素判据在那里恒为 0；只有真字体 + 真画布才能量“屏幕上还有没有字”。
        md_lines = ["# 预览滚动回归", "", "```text"]
        for i in range(30):
            md_lines.append(f"code line {i:02d} ======================================")
        md_lines += ["```", "", "尾部正文"]
        (workspace / "md_scroll.md").write_text("\n".join(md_lines), encoding="utf-8")
        open_quick(client, "md_scroll.md", settle=1.5)
        client.invoke("tab-md-preview", "click") if client.count("#tab-md-preview") else None
        if not client.count("#md-preview"):
            client.key("v", ctrl=True, shift=True)   # 快捷键兑底
        time.sleep(1.8)
        check(client.count("#md-preview") == 1, "Markdown 预览已打开")
        if client.count("#md-preview"):
            box = client.bounds("#md-preview")
            cx, cy = box["x"] + box["width"] / 2, box["y"] + box["height"] / 2
            # 三处采样：顶部、代码块中段（缺陷区）、再深一点
            max_scroll = float(client.get("md-preview").get("max_scroll") or 0.0)
            for frac in (0.0, 0.35, 0.6):
                client.set_props("md-preview", {"scroll": str(max_scroll * frac)})
                time.sleep(0.7)
                stats = client.get("md-preview").get("draw_stats") or "0|0"
                drawn, with_text = (int(x) for x in stats.split("|")[:2])
                groups = int(client.get("md-preview").get("painted_groups") or 0)
                check(drawn > 0, f"预览 {frac:.0%} 处遍历到了行（{drawn}）")
                # 核心断言：**绝大多数**行都有字可画。
                # 不能要求“全部”（空行/分隔线本就无字，那会让断言假阳性——实测 0% 处
                # 就有一行空行）；但也不能不查（旧缺陷下脚本中段全屏无字）。
                check(with_text * 4 >= drawn * 3,
                      f"预览 {frac:.0%} 处绝大多数行有文字（{with_text}/{drawn}）")
                # 组背景不得每行重画（旧实现下 = 组内行数，这正是盖字的手因）
                check(groups <= 2,
                      f"预览 {frac:.0%} 处组背景只画 {groups} 次（≤2；旧实现按行重画）")
            client.set_props("md-preview", {"scroll": "0"})

        # —— 10. 二进制文件：只读打开 + 保存拒绕（防“把乱码写回磁盘”）——
        #
        # 为何单列一条：功能面测试里二进制文件“能打开、显示 N 行”就算过，
        # 但真实风险是**它能被编辑并保存**。实测：输入 3 个字节后 Ctrl+S，
        # 文件就永久变成了乱码（原字节不可恢复）。
        bin_before = (workspace / "binary.bin").read_bytes()
        open_quick(client, "binary.bin")
        check(client.get("editor").get("read_only") == "true", "二进制文件以只读打开")
        bin_text_before = client.text("editor")
        client.invoke("editor", "focus")
        client.type_text("CORRUPT", "editor")
        time.sleep(0.3)
        check(client.text("editor") == bin_text_before, "只读态下键盘输入不改变内容")
        # 绕过只读强写一次文本，再保存：保存侧必须拦。
        client.set_props("editor", {"text": "CORRUPTED_BY_E2E"})
        time.sleep(0.3)
        client.key("s", ctrl=True)
        time.sleep(1.0)
        check((workspace / "binary.bin").read_bytes() == bin_before,
              "Ctrl+S 不改写二进制文件（字节级）")
        check("二进制" in client.text("status"), f"状态栏说明为何没保存（{client.text('status')}）")

        # —— 11. 增量暂存：勾“一处改动”真的只暂存那一处 ——
        #
        # 必须验到 git 侧（`git diff --cached`），而不是只看状态栏文案：
        # 这条链路最容易出的错是“勾了删行就只暂存了删除”——于是文件被改坏
        # （内容被删）而不是改成新值。
        lines = [f"line {i}" for i in range(1, 41)]
        (workspace / "multi.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        git("add", "multi.txt")
        git("commit", "-qm", "add multi")
        lines[1] = "line 2 CHANGED_A"
        lines[35] = "line 36 CHANGED_B"
        (workspace / "multi.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        # id 里带 `.` 会被选择器当 CSS 语法拆开，精确匹配要写 `[id=…]`。
        multi_selector = "[id=git-file-multi.txt]"
        if not client.count(multi_selector):
            open_scm_panel()
            if client.count("[id=scm-refresh-toolbar]"):
                client.invoke("scm-refresh-toolbar", "click")
                time.sleep(1.8)
        if client.count(multi_selector):
            client.invoke("git-file-multi.txt", "click")
            time.sleep(1.6)
            client.invoke("diff-staging-toggle", "click")
            time.sleep(1.0)
            check(client.get("diff-view").get("staging") == "true", "逐行暂存模式可开（属性可读）")
            client.invoke("diff-view", "clear_selection")
            time.sleep(0.5)
            # 第 0 个 hunk 的第一处改动（零坐标：动作面按 (hunk,line) 精确指定）
            client.invoke("diff-view", "toggle_line", "0,1")
            time.sleep(0.6)
            selected = client.get("diff-view").get("selected_lines")
            check(bool(selected), f"勾选行可读回（{selected}）")
            enabled = client.get("diff-stage-selected").get("enabled")
            # ⚠ `enabled` 是**布尔**（不是字符串 "true"）——属性面里布尔型直传。
            check(enabled is True, f"勾选后「暂存选中行」按钮解除禁用（enabled={enabled}）")
            client.invoke("diff-stage-selected", "click")
            time.sleep(2.5)
            cached = git("diff", "--cached")
            check("CHANGED_A" in cached, "暂存区含勾选的那处改动（含新增行）")
            check("CHANGED_B" not in cached, "未勾选的改动不进暂存区")
            # “只删不加”的识别：该处只有 `-line 2` 而**没有**对应的 `+line 2 CHANGED_A`。
            # （注意：整对形态下 `-line 2` 本来就该在——它是“删旧”那一半。）
            added_a = [l for l in cached.splitlines() if l.startswith("+") and "CHANGED_A" in l]
            removed_plain = [l for l in cached.splitlines() if l == "-line 2"]
            check(bool(added_a) and bool(removed_plain),
                  "该处是“整对改动”（既有删旧也有加新，不是把内容删掉）")
            git("reset", "-q")
        else:
            check(False, "变更面板应列出 multi.txt")

        # —— 12. 截图（人眼核验用）——
        shot = shots / "gbcode-usability.png"
        client.ok("capture", {"encode": "file", "path": str(shot)})
        print(f"  [截图] {shot}")
    finally:
        try:
            client.ok("app", {"action": "quit"})
        except Exception:
            pass
        time.sleep(0.4)
        try:
            process.kill()
        except Exception:
            pass
        shutil.rmtree(workspace, ignore_errors=True)

    print(f"\n失败 {len(failures)} 项")
    for item in failures:
        print(" -", item)
    return len(failures)


if __name__ == "__main__":
    sys.exit(main())
