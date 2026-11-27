#!/usr/bin/env python3
"""编辑器形态冒烟：焦点链路 + 文本送达 + 模态浮层铺满视口（控制通道，无头）。

为什么单列一个脚本：这三条都是"只能靠**真实控制通道往返**证明"的约定，组件级单测
证明不了——
  - **点击即聚焦**：点击路径是 `hit_test → focusable() && set_focus`，而键盘激活
    （`activate()`）又要求先有焦点。默认不可聚焦时这是个死循环，单测直接调
    `set_focused` 看不出来。
  - **文本送到焦点元素**：`input.text` 若目标不可聚焦会静默落到旧焦点元素上
    （写错元素比报错危险）——必须按 id 断到文本真的进了编辑器。
  - **模态浮层铺满视口**：`OverlayLayout::FillViewport` 之前，应用只能每帧注入视口
    尺寸（workaround）；本脚本断的是"应用没注入也能铺满"。

用法: python3 tools/st_editor_smoke.py [gbcode|gallery|all] [--shots DIR]
退出码 0 = 全部通过。
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import socket
import struct
import subprocess
import sys
import time

# 控制通道握手标识（与服务端 st::control::kAgentOnlyNotice 同一句）：
# 本脚本在 hello 里原样带上，服务端据此认出“这是霜天子侧客户端”。
AGENT_HANDSHAKE = (
    "仅霜天子Agent 可连接本控制通道：请勿手工拼装 TCP 协议，改用霜天子工具（shuangtian_run / shuangtian_tree / shuangtian_capture / shuangtian_call …）"
)

# 非 UTF-8 控制台自愈：GBK 代码页下 print 中文/符号（如 '⇄'）会 UnicodeEncodeError 崩整跑；
# 已是 UTF-8 则不动，残余不可编字符以 '?' 兜底（宁可错印不崩）。
if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROFILE = os.environ.get("ST_PROFILE", "debug")

failures: list[str] = []


def check(condition: bool, message: str) -> bool:
    print(("  OK   " if condition else "  FAIL ") + message, flush=True)
    if not condition:
        failures.append(message)
    return condition


class Client:
    """**单连接**会话：控制协议要求每条连接先 `hello` 握手，后续请求复用同一连接。"""

    def __init__(self, port: int, token: str) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.counter = 0
        hello = self.call("hello", {"token": token, "agent": AGENT_HANDSHAKE})
        if not hello.get("ok"):
            raise RuntimeError(f"hello 失败: {hello.get('error')}")
        self.screen = (hello.get("result") or {}).get("screen", {})

    def call(self, method: str, params: dict | None = None) -> dict:
        self.counter += 1
        body = json.dumps({"id": self.counter, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        length = struct.unpack(">I", self._read_exact(4))[0]
        return json.loads(self._read_exact(length).decode())

    def ok(self, method: str, params: dict | None = None) -> dict:
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise RuntimeError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def _read_exact(self, size: int) -> bytes:
        buffer = b""
        while len(buffer) < size:
            chunk = self.sock.recv(size - len(buffer))
            if not chunk:
                raise RuntimeError("连接被关闭")
            buffer += chunk
        return buffer

    def close(self) -> None:
        self.sock.close()


def start(app: str, shots: pathlib.Path,
          extra_args: list[str] | None = None) -> tuple[subprocess.Popen, Client]:
    control = shots / f"editor-smoke-{app}-control.json"
    control.unlink(missing_ok=True)
    # Windows 产物带 .exe 后缀（此前只拼无后缀路径，Windows 上永远报「未找到」）
    executable = ROOT / "build" / PROFILE / "bin" / app
    if sys.platform == "win32":
        executable = executable.with_name(executable.name + ".exe")
    if not executable.exists():
        raise RuntimeError(f"未找到 {executable}（先 st build {app}）")
    log = open(shots / f"editor-smoke-{app}.log", "wb")
    process = subprocess.Popen(
        [str(executable), "--headless", "--control-port", "0", "--control-file", str(control),
         "--shots", str(shots), *(extra_args or [])],
        stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True,
    )
    for _ in range(120):
        if control.exists():
            info = json.loads(control.read_text())
            if info.get("port"):
                try:
                    return process, Client(int(info["port"]), info.get("token", ""))
                except (OSError, RuntimeError):
                    pass
        time.sleep(0.25)
    raise RuntimeError(f"{app} 未在 30 秒内就绪")


def stop(process: subprocess.Popen) -> None:
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()


def smoke_gbcode(shots: pathlib.Path) -> None:
    print("[gbcode] 焦点链路 + 文本送达", flush=True)
    # ⚠ **必须先给一个要打开的文件**：gbcode 的编辑器是**条件渲染**的——
    # 没有活动标签时编辑区只有 `empty-hint`（「打开一个文件或文件夹开始」），
    # `#editor` 根本不在树上。旧版这里直接 `get editor`，在空态会话下必然是
    # “未找到元素: editor”（既不是回归，也不是环境问题，而是**前提没造**）。
    #
    # 用自己的临时文件而不是仓里的某个源文件：被测断言会往里插一个 `Z` 再退格，
    # 拿真文件当夹具就等于“跑一次冒烟往工作区写一笔”。
    workspace = shots / "gbcode-smoke-workspace"
    workspace.mkdir(parents=True, exist_ok=True)
    sample = workspace / "smoke-sample.cpp"
    sample.write_text("int main() {\n  return 0;\n}\n", encoding="utf-8")
    process, client = start("gbcode", shots, ["--workspace", str(workspace),
                                             "--open", str(sample)])
    try:
        # 启动即开文件 → 编辑器要等标签建好才上树；按界面条件等，不拍 sleep。
        wait_for(client, "#editor", "编辑器")
        state = client.ok("get", {"id": "editor"})
        props = state.get("props", {})
        # 示例启动即 `root->set_focus(editor_ptr)`：初始应当是已聚焦
        check(props.get("focused") is True, "示例启动即聚焦编辑器（root.set_focus）")
        bounds = state["bounds"]
        center = {"x": bounds["x"] + bounds["width"] / 2, "y": bounds["y"] + bounds["height"] / 2}

        # 先显式失焦：让下面的点击成为"从无焦点到有焦点"的真实路径
        client.ok("invoke", {"id": "editor", "action": "blur"})
        check(client.ok("get", {"id": "editor"}).get("props", {}).get("focused") is False,
              "invoke blur 后失焦（focused=false）")

        client.ok("input.mouse", {"kind": "click", **center, "button": 1})
        check(client.ok("get", {"id": "editor"}).get("props", {}).get("focused") is True,
              "鼠标点击编辑区即聚焦（点击路径不依赖键盘激活）")

        before = client.ok("get", {"id": "editor"}).get("props", {}).get("text", "")
        client.ok("input.text", {"id": "editor", "text": "Z"})
        after = client.ok("get", {"id": "editor"}).get("props", {}).get("text", "")
        check(after != before and "Z" in after, "聚焦后 input.text 插入字符（文本送达焦点元素）")

        client.ok("input.key", {"kind": "press", "key": "Backspace"})
        restored = client.ok("get", {"id": "editor"}).get("props", {}).get("text", "")
        check(restored == before, "Backspace 复原（插入-删除闭环）")

        shot = client.ok("capture", {"id": "editor", "encode": "file",
                                     "path": str(shots / "editor-smoke-cursor.png")})
        check(bool(shot.get("path")), f"编辑区截图已落盘: {shot.get('path')}（人工看光标竖线）")

        # 焦点环不被键盘陷阱吞噬：在普通按钮上 Tab 必须能走
        # （旧断言指向 `viewer`——那是重写前的只读视图，已不存在：陈旧断言按诚实原则改对现状）。
        client.ok("invoke", {"id": "activity-search", "action": "focus"})
        focused_before = client.ok("input.key", {"kind": "press", "key": "Shift"}).get("focused")
        check(focused_before == "activity-search", f"活动栏按钮可聚焦: {focused_before}")
        after_tab = client.ok("input.key", {"kind": "press", "key": "Tab"}).get("focused")
        check(after_tab not in ("", "activity-search"), f"按钮上 Tab 在焦点环内前进（移到 {after_tab}）")
        client.close()
    finally:
        stop(process)


def wait_for(client: Client, selector: str, what: str, tries: int = 60) -> None:
    """按**界面条件**等待元素出现（不靠拍脑袋的 sleep 值）。

    切页/切分类会重建子树，而控制通道的 `invoke` 在回调里就返回了——新页的节点
    可能还没挂上去。轮询 `find` 是唯一稳定的判据（`count > 0`），
    而固定 sleep 在慢构建档或忙的机器上就会间歇失败。
    """
    for _ in range(tries):
        found = client.ok("find", {"selector": selector, "limit": 1})
        if found.get("count", len(found.get("matches", []))) > 0:
            return
        time.sleep(0.05)
    raise RuntimeError(f"{what}未出现（等待 {selector} 超时）")


def smoke_gallery(shots: pathlib.Path) -> None:
    print("[gallery] 模态浮层（FillViewport）+ Esc 关闭", flush=True)
    process, client = start("gallery", shots)
    try:
        screen = client.screen
        # 画廊已改成**分区多页**（概览/组件/属性面/自检台/…），而对话框展位在
        # 「组件 → 下拉与浮层」分类里。步骤必须按**用户路径**走完这两跳：
        # 不切页/不切分类时卡片虽然还在树上，但 `visible=false`，
        # 点它不会有回调——症状是「invoke 成功但对话框没出现」。
        client.ok("invoke", {"id": "nav-components", "action": "click"})
        # 切页是**重建页面**（声明式/手搭都一样）：`nav-components` 返回时新页的
        # 子树可能还没挂完，立刻 invoke 分类按钮会 not_found。所以按界面条件等，
        # 不靠 sleep 的拍脑袋值（同一套脚本在慢构建档上就能稳定）。
        wait_for(client, "#exhibit-tab-select", "组件页分类栏")
        # ⚠ 这一段守的是一个**已经真实发生过的崩溃**，不能只点一次。
        #
        # 分类按钮的 `on_click` 里要读「当前分类」与「全部按钮指针」，而这两样原本是
        # `build_components()` 的局部变量、被 lambda 按引用捕获——函数返回后全是悬垂引用，
        # 点一下就是 `0xC0000005`（debug 档抓到的栈是 `pages.cpp` 的 `set_variant`）。
        #
        # 所以多切几类：一次可能只是踩到"回收后还没被覆盖"的旧内存，看起来还正常
        # （dev 档就表现为「点了没反应」），连切几轮才能把真问题露出来。
        for section in ("select", "toggle", "data", "advanced", "basic"):
            client.ok("invoke", {"id": f"exhibit-tab-{section}", "action": "click"})
        counts = client.ok("get", {"id": "exhibit-search-state"})
        # `get` 的顶层字段就是 `text`（`Text` 类元素没有 `value`——`value` 是
        # Input/Slider 那一族的）。读错字段名拿到空串，断言会“假红”而不是报错。
        count_text = counts.get("text", "")
        check("4 个展示位" in count_text,
              f"连切五类后回到「基础与文字」（应回 4 个展示位，实为「{count_text}」）")
        client.ok("invoke", {"id": "exhibit-tab-select", "action": "click"})
        client.ok("invoke", {"id": "btn-open-dialog", "action": "click"})
        dialog = client.ok("get", {"id": "demo-dialog"})
        bounds = dialog.get("bounds", {})
        check(bounds.get("width") == screen.get("width") and bounds.get("height") == screen.get("height"),
              f"对话框遮罩铺满视口（{bounds.get('width')}x{bounds.get('height')} == "
              f"{screen.get('width')}x{screen.get('height')}；应用侧未注入视口尺寸）")
        card = dialog.get("props", {}).get("card", "")
        check(card not in ("", "0,0,0,0"), f"卡片已定位: {card}")

        client.ok("input.key", {"kind": "press", "key": "Escape"})
        found = client.ok("find", {"selector": "Dialog", "limit": 5})
        check(not found.get("matches", found.get("items", [])),
              "Esc 关闭对话框（选择器已查不到 Dialog）")
        client.close()
    finally:
        stop(process)


def main() -> int:
    parser = argparse.ArgumentParser(description="编辑器形态冒烟（控制通道，无头）")
    parser.add_argument("target", nargs="?", default="all",
                        choices=["gbcode", "gallery", "all"])
    parser.add_argument("--shots", default=None, help="截图与日志目录（默认 build/editor-smoke）")
    args = parser.parse_args()
    shots = pathlib.Path(args.shots) if args.shots else ROOT / "build" / "editor-smoke"
    shots.mkdir(parents=True, exist_ok=True)

    targets = ["gbcode", "gallery"] if args.target == "all" else [args.target]
    try:
        for target in targets:
            if target == "gbcode":
                smoke_gbcode(shots)
            else:
                smoke_gallery(shots)
    except RuntimeError as error:
        check(False, str(error))
    print(("冒烟通过" if not failures else f"冒烟失败 {len(failures)} 项"), flush=True)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
