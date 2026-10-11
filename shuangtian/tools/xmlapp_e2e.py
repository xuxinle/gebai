"""xmlapp（纯 XML + JS 示例）端到端回归。

验证的是**通路的三个接缝**，任一处坏掉应用就不成立：
① XML → 界面树（标签/属性真的建出了元素）；
② JS → C++（脚本调的 `app.tasks.*` 真落到了 C++ 的数据）；
③ JS 动态建结构（`$('#x').append(...)` 真在树上加/删了元素）。

跑法：py -3 tools/xmlapp_e2e.py
"""

import base64
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / "xmlapp.exe"
UI_DIR = ROOT / "examples" / "xmlapp" / "ui"

sys.path.insert(0, str(ROOT / "tools"))
from gbcode_hint_e2e import Client, wait_until  # noqa: E402


def log(message):
    print(message, flush=True)


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}（先 st build xmlapp）")
        return 2
    shots = ROOT / "tmp" / "xmlapp-e2e"
    shutil.rmtree(shots, ignore_errors=True)
    shots.mkdir(parents=True, exist_ok=True)
    ctl = shots / "ctl.json"
    handle = open(shots / "log.txt", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(shots), "--ui", str(UI_DIR)],
        stdout=handle, stderr=subprocess.STDOUT)
    failures = []

    def check(ok, label):
        log(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    try:
        client = None
        for _ in range(240):
            if ctl.exists():
                try:
                    info = json.loads(ctl.read_text(encoding="utf-8"))
                    if info.get("port"):
                        client = Client(info["port"], info.get("token", ""))
                        client.ok("hello")
                        break
                except Exception:  # noqa: BLE001
                    pass
            time.sleep(0.25)
        if client is None:
            log("控制通道没起来：")
            log((shots / "log.txt").read_text(encoding="utf-8", errors="replace")[-1500:])
            return 1

        def find(selector):
            return client.ok("find", {"selector": selector}).get("matches") or []

        def text_of(selector):
            matches = find(selector)
            return matches[0].get("text") if matches else None

        # ① XML → 界面树：XML 里写的 id 都该能被选择器寻址。
        for selector in ("#header-title", "#draft", "#add", "#list", "#footer", "#clear-done"):
            check(len(find(selector)) == 1, f"XML 元素在场：{selector}")
        check(text_of("#header-title") == "任务清单", f"标题文本来自 XML（{text_of('#header-title')!r}）")
        check(text_of("#counter") == "共 0 项 · 完成 0",
              f"② JS 从 C++ 取到初始计数（{text_of('#counter')!r}）")

        # ③ JS 动态建结构：往里输入文本再点「添加」。
        client.ok("set", {"id": "draft", "props": {"value": "写一个纯 XML 应用"}})
        client.ok("invoke", {"id": "add", "action": "click"})
        # 行是 JS 建的：判据用**行内元素出现在场**（`find` 的 match 不带 children 字段，
        # 所以不能按 list 的子元素数断言——实测这里踩过一次假失败）。
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if find("#text-0"):
                break
            time.sleep(0.2)
        check(bool(find("#text-0")), "③ JS 动态 add 出行（行内文本元素在场）")
        check(text_of("#counter") == "共 1 项 · 完成 0",
              f"计数跟着更新（{text_of('#counter')!r}）")
        # 行里的子元素也是 JS 建的（Checkbox/Text/Button）。
        check(len(find("#text-0")) == 1, "JS 建的行内有文本元素")

        # 再添加一条：第二行在场，且**第一行的内容没丢**（验证重建是"清空 + 重建"而不是叠加）。
        client.ok("set", {"id": "draft", "props": {"value": "第二条"}})
        client.ok("invoke", {"id": "add", "action": "click"})
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if find("#text-1"):
                break
            time.sleep(0.2)
        check(bool(find("#text-1")), "第二条也建出来了")
        check(len(find("#text-0")) == 1, "第一行仍在且只有一份（重建无叠加残留）")

        # 勾选第一条 → 完成计数变化（JS → C++ → JS 的闭环）。
        #
        # ⚠ 用**真实鼠标点击**而不是 `invoke click`：`Checkbox` 的切换不是一等动作
        #（`invoke_action` 只覆盖 Button 这类有动作语义的组件），走鼠标才是用户路径
        #——顺带把"点得到"也验了（坐标错的话这里就红）。
        box = find("#check-0")
        check(bool(box), "勾选框在场")
        if box:
            rect = box[0]["bounds"]
            client.ok("input.mouse", {"kind": "click", "x": rect["x"] + rect["width"] / 2,
                                      "y": rect["y"] + rect["height"] / 2, "button": 1})
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if (text_of("#counter") or "").find("完成 1") >= 0:
                break
            time.sleep(0.2)
        check((text_of("#counter") or "").find("完成 1") >= 0,
              f"勾选后完成计数 +1（{text_of('#counter')!r}）")

        # 「清除已完成」→ 只剩一条（C++ 侧真删了）。这个是 Button，`invoke click` 就是它的动作。
        client.ok("invoke", {"id": "clear-done", "action": "click"})
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if (text_of("#counter") or "").startswith("共 1 项"):
                break
            time.sleep(0.2)
        check((text_of("#counter") or "").startswith("共 1 项"),
              f"清除已完成后剩 1 项（{text_of('#counter')!r}）")

        # 布局：底部那行应当贴在窗口下沿（`#list` 的 grow 撑开了中间）。
        #
        # ⚠ 窗口高度从**根元素**的几何拿（`metrics` 顶层没有 `screen`——那是握手
        # `hello` 的字段；实测按 `metrics.screen.height` 取会拿到 0 而误报失败）。
        root_box = find("#root")
        footer = find("#footer")
        check(bool(root_box) and bool(footer), "拿到底部行与根的几何")
        if root_box and footer:
            root_rect = root_box[0]["bounds"]
            window_bottom = root_rect["y"] + root_rect["height"]
            box = footer[0]["bounds"]
            bottom_gap = window_bottom - (box["y"] + box["height"])
            check(bottom_gap <= 2,
                  f"底部行贴下沿（距下沿 {bottom_gap:.0f}px，内容区底 {window_bottom:.0f}）")

        (shots / "final.png").write_bytes(base64.b64decode(client.ok("capture", {})["base64"]))
        check((shots / "final.png").exists(), "截图落盘")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

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
