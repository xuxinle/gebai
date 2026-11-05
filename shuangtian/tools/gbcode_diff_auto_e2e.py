"""gbcode 差异视图「一体 + 自动」能力 E2E（无头 + 控制通道）。

覆盖 2026-11-05 用户要求的那一套语义：

1. **打开对比视图自动跳到第一个差异**（不是停在文件第 1 行）；
2. **右栏可编辑**、改了就地**自动重算对比**（与暂存区/HEAD 比当前内容）；
3. **上下差异跳转**（F7 / Shift+F7 → `next_hunk`/`previous_hunk`）；
4. **上下有差异的文件跳转**（Alt+F7 / Alt+Shift+F7 → `next-diff-file` 键位）；
5. **标签栏预览按钮激活时高亮**（`Tabs` 的 `trailing` 属性面读 activation 位）；
6. **启动默认不打开终端**（首帧 `#terminal` 不可见、且底部面板收起）；
7. **代码编辑器空白滚动**（末行可滚到视口顶部）与**滚动条光标**（`scroll` 而非 `text`）。

跑法：`py -3 tools/gbcode_diff_auto_e2e.py`
自带真 git 仓库（临时目录，用完即删），全程控制通道驱动。
"""

import base64
import json
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / ("gbcode.exe" if os.name == "nt" else "gbcode")


def log(msg):
    print(msg, flush=True)


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)


class Client:
    """控制通道客户端（长度前缀 JSON，与 `gbcode_compare_e2e.py` 同一套）。"""

    def __init__(self, port, token):
        self.port = port
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.rid = 0
        self.token = token

    def call(self, method, params=None):
        try:
            return self._call(method, params)
        except ConnectionAbortedError:
            self.sock.close()
            self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=30.0)
            self.sock.settimeout(30.0)
            self.rid = 0
            self._call("hello")
            return self._call(method, params)

    def _call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        header = self._read(4)
        payload = self._read(struct.unpack(">I", header)[0])
        return json.loads(payload)

    def _read(self, size):
        buf = b""
        while len(buf) < size:
            chunk = self.sock.recv(size - len(buf))
            if not chunk:
                raise RuntimeError("控制通道提前关闭")
            buf += chunk
        return buf

    def ok(self, method, params=None):
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]

    def set_prop(self, element_id, name, value):
        return self.ok("set", {"id": element_id, "props": {name: value}})

    def invoke(self, element_id, action, argument=""):
        return self.ok("invoke", {"id": element_id, "action": action, "argument": argument})

    def click(self, selector):
        found = self.ok("find", {"selector": selector})
        matches = found.get("matches") or []
        if not matches:
            raise AssertionError(f"找不到元素 {selector}")
        bounds = matches[0]["bounds"]
        self.ok("input.mouse", {"kind": "click",
                                "x": bounds["x"] + bounds["width"] / 2.0,
                                "y": bounds["y"] + bounds["height"] / 2.0, "button": 1})
        time.sleep(0.4)

    def key(self, name, ctrl=False, shift=False, alt=False):
        self.ok("input.key", {"key": name, "ctrl": ctrl, "shift": shift, "alt": alt})
        time.sleep(0.35)

    def screenshot(self, path):
        result = self.ok("capture", {})
        data = (result.get("base64") or result.get("png_base64") or result.get("data")
                or result.get("png"))
        if not data:
            raise AssertionError(f"截图返回里没有图像数据：{sorted(result.keys())}")
        pathlib.Path(path).write_bytes(base64.b64decode(data))


def wait_until(fn, timeout=25.0, interval=0.25, what="条件"):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = fn()
            if last:
                return last
        except Exception as exc:  # noqa: BLE001
            last = exc
        time.sleep(interval)
    raise AssertionError(f"等待超时：{what}（最后：{last!r}）")


def start(workspace, shots, extra_args=()):
    ctl = shots / "gbcode-ctl.json"
    ctl.unlink(missing_ok=True)
    log_file = open(shots / "gbcode-diff-auto.log", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(shots), "--tool-root", str(ROOT), "--workspace", str(workspace),
         *extra_args],
        stdout=log_file, stderr=subprocess.STDOUT)
    for _ in range(160):
        if ctl.exists():
            try:
                info = json.loads(ctl.read_text(encoding="utf-8"))
                if info.get("port"):
                    client = Client(info["port"], info.get("token", ""))
                    client.ok("hello")
                    return proc, client, info
            except Exception:  # noqa: BLE001
                pass
        time.sleep(0.25)
    proc.kill()
    raise AssertionError("gbcode 控制通道没起来（看 tmp/gbcode-diff-auto-shots/gbcode-diff-auto.log）")


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}；先 `st build gbcode --profile dev`")
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_diffauto_"))
    repo = work / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "e2e@local")
    git(repo, "config", "user.name", "E2E")
    # 一份**足够长**的文件 + **大量分散的改动**：这样差异视图总共有几十个 hunk
    #（显示行远超视口），“打开时跳回第一个差异”才有可观测的位移。
    long_lines = [f"line {index}" for index in range(1, 401)]
    (repo / "alpha.txt").write_text("\n".join(long_lines) + "\n", encoding="utf-8")
    (repo / "beta.txt").write_text("bee\n", encoding="utf-8")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "first")
    # 每 20 行改一处，共 20 个 hunk；行距 20 > 上下文 3+3，不会被合并成一个块。
    for row in range(19, 400, 20):
        long_lines[row] = f"line {row + 1} CHANGED"
    (repo / "alpha.txt").write_text("\n".join(long_lines) + "\n", encoding="utf-8")
    (repo / "beta.txt").write_text("bee changed\n", encoding="utf-8")
    # 一个 Markdown 文件：用 `--open` 启动它，让标签栏出现**预览钮**（只有 md 才有）。
    (repo / "note.md").write_text("# title\n\ntext\n", encoding="utf-8")

    shots = ROOT / "tmp" / "gbcode-diff-auto-shots"
    shots.mkdir(parents=True, exist_ok=True)

    failures = []

    def check(ok, label):
        log(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    proc = None
    try:
        proc, client, _info = start(repo, shots, ["--open", str(repo / "note.md")])

        # ⓪ 标签栏预览钮的激活态（用户要求“预览按钮激活时要颜色高亮”）。
        #
        # 断言的是**数据面**：`Tabs` 的 `trailing` 属性读的是 `图标:激活:可用`。
        # 高亮颜色本身是像素（组件单测里钉了 active → `primary_soft` 底 +
        # `primary` 图标），这里证明的是“开关一开，那个槽真的变成激活态”。
        trailing = wait_until(
            lambda: client.props("editor-tabs").get("trailing", "") if client.count("#editor-tabs") else "",
            what="标签栏出现（带预览钮）")
        check("eye" in trailing, f"Markdown 文件标签栏有预览钮（trailing = {trailing}）")
        client.key("v", ctrl=True, shift=True)   # Ctrl+Shift+V 开预览
        wait_until(lambda: "eye:1:1" in client.props("editor-tabs").get("trailing", ""),
                   what="预览按钮变为激活态（高亮）")
        check("eye:1:1" in client.props("editor-tabs").get("trailing", ""),
              "开启预览后预览钮为激活态（eye:1:1 → 高亮）")
        client.key("v", ctrl=True, shift=True)   # 关回去，不影响后面的差异检查
        wait_until(lambda: "eye:0:1" in client.props("editor-tabs").get("trailing", ""),
                   what="预览按钮回到常态")

        # ① 启动默认不打开终端（用户要求）。
        #
        # 判据看**尺寸**而不是 `visible`：面板收起是“隐藏子树但不拆”
        #（`SplitView::set_second_hidden`），所以 `#terminal` 仍可见、但尺寸是 0×0——
        # 它不占屏幕、不参与命中，这才是“没打开”的真实含义。
        terminal_bounds = {"width": -1, "height": -1}
        if client.count("#terminal"):
            found = client.ok("find", {"selector": "#terminal"})
            match = (found.get("matches") or [{}])[0]
            terminal_bounds = match.get("bounds") or terminal_bounds
        layout_hidden = terminal_bounds.get("height", 0) <= 0.0
        split = client.props("bottom-split")
        check(layout_hidden and split.get("second_hidden") == "true",
              f"启动时终端未展开（terminal 尺寸 {terminal_bounds.get('width')}×"
              f"{terminal_bounds.get('height')}，second_hidden={split.get('second_hidden')}）")

        # ② 打开差异：变更面板 → alpha.txt。
        client.click("#activity-scm")
        wait_until(lambda: client.count("Button[id=git-file-alpha.txt]") == 1, what="变更面板出现 alpha.txt")
        client.click("Button[id=git-file-alpha.txt]")
        wait_until(lambda: client.count("#diff-view") == 1, what="差异视图出现")
        time.sleep(0.6)

        props = client.props("diff-view")
        hunks = int(props.get("hunks", "0") or 0)
        check(hunks >= 5, f"差异块数 = {hunks}（期望 ≥ 5）")

        # ③ 打开即定位到第一个差异：当前块 = 0。
        current = props.get("current_hunk", "")
        check(current == "0", f"打开后当前块 = {current}（期望 0）")

        # ③b **打开就自动跳到第一处差异**（用户报“点了没跳”）。
        #
        # 判据看**视口**而不是 `current_hunk`：`current_hunk` 是 0 但视口停在顶部
        # 恰恰是那个缺陷的形态（`scroll_to_line` 被 `max_scroll_y()` 夹成 0——
        # 因为构建期组件的 `bounds_` 还是 0）。
        # 本仓库的差异块都在前面几行之后，所以“跳了”等价于“scroll_y > 0”。
        scroll_open = float((props.get("scroll") or "0").split(",")[0])
        hunk_line = int(props.get("current_hunk_line", "0") or 0)
        check(hunk_line > 3, f"第一处差异不在文件开头（第 {hunk_line} 行，才能验“真的跳了”）")
        check(scroll_open > 0.0,
              f"打开即跳第一处：视口已下移（scroll_y = {scroll_open:.1f}，期望 > 0）")

        # ④ 上下差异跳转（F7 / Shift+F7 的等价动作），顺便把视口推离顶部。
        for _ in range(hunks - 1):
            client.invoke("diff-view", "next_hunk")
        after = client.props("diff-view")
        check(after.get("current_hunk") == str(hunks - 1),
              f"逐个下一处 → 末块 {after.get('current_hunk')}（期望 {hunks - 1}）")
        scrolled = float((after.get("scroll") or "0").split(",")[0])
        check(scrolled > 0.0, f"跳到末块后视口已下移（scroll_y = {scrolled:.1f}）")

        # ④b **工具条上的「上一处 / 下一处」按钮**——必须单独测（用户报“按了没用”）。
        #
        # 为何以前没发现：历史上这里只调 `diff-view` 的**动作面**（上面那行），
        # 而按钮走的是另一条路（`build` 里的 `custom<Button>`）。两个真实缺陷就躲在差异里：
        #  ① `set_enabled(diff_view_ptr != nullptr)` —— 指针在本帧 `build()` 开头刚被置空，
        #     而赋值在同一帧稍后的 `custom<DiffView>` 里 ⇒ 按钮**永远 disabled**，点了没反应；
        #  ② 序号文本同样读 `diff_view_ptr` ⇒ 永远空串。
        # 所以这里断言三件事：按钮可点、跳转真的发生、序号跟着变。
        client.invoke("diff-view", "goto_hunk", "0")
        time.sleep(0.3)
        prev_btn = client.props("diff-prev-hunk")
        next_btn = client.props("diff-next-hunk")
        check(prev_btn.get("enabled") is True,
              f"「上一处」按钮可点（enabled={prev_btn.get('enabled')}）")
        check(next_btn.get("enabled") is True,
              f"「下一处」按钮可点（enabled={next_btn.get('enabled')}）")
        client.invoke("diff-next-hunk", "click")
        wait_until(lambda: client.props("diff-view").get("current_hunk") == "1",
                   what="点「下一处」跳到第 2 处")
        check(client.props("diff-view").get("current_hunk") == "1",
              "点「下一处」→ 当前块 0 → 1")
        position_text = client.props("diff-hunk-position").get("text", "")
        check(position_text.startswith("第 2"),
              f"工具条序号跟着变（实际 {position_text!r}，期望“第 2 / N 处”）")
        client.invoke("diff-prev-hunk", "click")
        wait_until(lambda: client.props("diff-view").get("current_hunk") == "0",
                   what="点「上一处」回到第 1 处")
        check(client.props("diff-view").get("current_hunk") == "0",
              "点「上一处」→ 当前块 1 → 0")

        # ⑤ **重新打开同一文件 → 回到第一个差异**（这正是用户要的“打开对比视图
        #    时自动跳转到第一个差异”：否则会停在上次的位置）。
        client.click("Button[id=git-file-alpha.txt]")
        wait_until(lambda: client.props("diff-view").get("current_hunk") == "0",
                   what="重新打开差异后回到第一个差异")
        reopened = client.props("diff-view")
        check(reopened.get("current_hunk") == "0",
              f"重新打开 → 当前块 = {reopened.get('current_hunk')}（期望 0）")
        check(float((reopened.get("scroll") or "0").split(",")[0]) < scrolled,
              "重新打开 → 视口回到第一个差异附近（scroll_y 回退）")

        # ⑤b **双击变更项不能把内容弄坏**（用户报“双击后左侧内容空白”）。
        #
        # 双击会走两次 `on_click`（Click + DoubleClick 都触发 `Button::activate`）。
        # 旧实现第二次 `open_diff` 把 `diff_pending_path_` 改了、而 key 与第一次相同
        # ⇒ 声明式不重组，但“自动重算”挂在 `diff_auto_refresh_` 上——于是拿**新路径**
        # 去配**旧编辑器内容**，摘要变成“（新增文件）+200 −0”、左栏全是空填充行。
        for target in ("beta.txt", "alpha.txt"):
            selector = f"Button[id=git-file-{target}]"
            bounds = client.ok("find", {"selector": selector})["matches"][0]["bounds"]
            cx = bounds["x"] + bounds["width"] / 2.0
            cy = bounds["y"] + bounds["height"] / 2.0
            for _ in range(2):   # 双击 = 两次 click
                client.ok("input.mouse", {"kind": "click", "x": cx, "y": cy, "button": 1})
                time.sleep(0.12)
            time.sleep(0.9)
            double = client.props("diff-view")
            left_text = client.props("diff-left").get("text") or ""
            check("新增文件" not in (double.get("summary") or ""),
                  f"双击 {target} 后不应变“新增文件”（实际 {double.get('summary')!r}）")
            # 左栏必须是**真内容**（不是对齐填充行）：填充行会让 strip 后几乎为空。
            # 阈值取 1 字而不是 100：本仓库的 beta.txt 本来就只有一行（“bee”），
            # 拿一个绝对字数当判据会把“文件小”误判成“内容丢了”。
            check(len(left_text.strip()) > 0,
                  f"双击 {target} 后左栏有真内容（strip 后 {len(left_text.strip())} 字）")
            right_text = client.props("diff-right").get("text") or ""
            # 两栏行数必须一致（并列视图的对齐不变式）——比字数更精确，
            # 而且不受“改动行长度差”影响。
            check(left_text.count("\n") == right_text.count("\n"),
                  f"双击 {target} 后两栏行数一致（{left_text.count(chr(10))} vs "
                  f"{right_text.count(chr(10))}）")

        # ⑥ 上下有差异的文件跳转（Alt+F7 家族）。
        client.key("f7", alt=True)
        wait_until(lambda: "beta.txt" in client.props("diff-view").get("summary", ""),
                   what="Alt+F7 跳到下一个有差异的文件（beta.txt）")
        check("beta.txt" in client.props("diff-view").get("summary", ""),
              "Alt+F7 → 下一个有差异的文件（beta.txt）")
        client.key("f7", alt=True, shift=True)
        wait_until(lambda: "alpha.txt" in client.props("diff-view").get("summary", ""),
                   what="Alt+Shift+F7 回到上一个有差异的文件（alpha.txt）")
        check("alpha.txt" in client.props("diff-view").get("summary", ""),
              "Alt+Shift+F7 → 上一个有差异的文件（alpha.txt）")

        # ⑦ 右栏可编辑 + 改了自动重算对比。
        editable = client.props("diff-view").get("editable", "")
        check(editable == "true", f"差异视图右栏可编辑（editable = {editable}）")
        before_hunks = client.props("diff-view").get("hunks", "")
        before_additions = client.props("diff-view").get("additions", "")
        # 在右栏任意位置输入一行新内容（走控制通道的文本输入：焦点已在右栏上）。
        client.invoke("diff-right", "focus")
        client.ok("input.text", {"text": "brand new line\n"})
        # 防抖后自动重算：等 hunks/additions 发生变化（最多 5s）。
        changed = wait_until(
            lambda: client.props("diff-view").get("additions", "") != before_additions
            or client.props("diff-view").get("hunks", "") != before_hunks,
            timeout=8.0, interval=0.3, what="编辑后自动重算对比")
        check(bool(changed),
              f"编辑右栏后自动重算（+{before_additions} → "
              f"+{client.props('diff-view').get('additions', '')}）")

        # ⑧ 截图（人可复核差异视图 + 标签栏）。
        shot = shots / "diff-auto.png"
        client.screenshot(shot)
        check(shot.exists() and shot.stat().st_size > 2000, f"截图落盘 {shot}")
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(work, ignore_errors=True)

    log("")
    if failures:
        log(f"失败 {len(failures)} 项：")
        for item in failures:
            log("  - " + item)
        return 1
    log("全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
