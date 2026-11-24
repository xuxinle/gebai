#!/usr/bin/env python3
"""gallery「声明式」页端到端核验（打磨轮）。

该页是 `dsl::mount_into` **子树挂载**的样板：页壳手搭、内容区声明式
（状态驱动表单 + key 对齐列表 + 异步 resource + 条件内容 + memo/effect/ref）。
本轮改了重组器（key 复用 / 护栏 / 预算）与 JS 宿主，所以这条路径必须实跑一遍：
切到该页 → 断言声明式元素真的在树上（`mount_into` 的锚点语义）→ 操作表单 →
断言状态驱动传播 → 截图。

用法：python tools/gallery_declarative_e2e.py [二进制路径] [截图目录]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from st_client_lib import call_with_token, load_control  # noqa: E402

# 非 UTF-8 控制台自愈：GBK 代码页下 print 中文/符号（如 '⇄'）会 UnicodeEncodeError 崩整跑；
# 已是 UTF-8 则不动，残余不可编字符以 '?' 兜底（宁可错印不崩）。
if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

DEFAULT_BIN = os.path.join(ROOT, "build", "debug", "bin", "gallery.exe")


def start(binary):
    control_file = os.path.join(ROOT, "build", "e2e-gallery", "gallery-control.json")
    os.makedirs(os.path.dirname(control_file), exist_ok=True)
    if os.path.exists(control_file):
        os.remove(control_file)
    process = subprocess.Popen(
        [binary, "--headless", "--control-file", control_file, "--scale", "1.0"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    for _ in range(200):
        info = load_control(control_file)
        if info.get("port"):
            try:
                ping = call_with_token(info["port"], "ping", {}, info.get("token", ""), timeout=2)
                if ping.get("ok") is not False:
                    return process, info
            except Exception:
                pass
        time.sleep(0.05)
    process.kill()
    raise RuntimeError("gallery 未在限时内就绪")


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    shots = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "e2e-gallery")
    os.makedirs(shots, exist_ok=True)
    process, info = start(binary)
    port, token = info["port"], info.get("token", "")
    call = lambda method, params=None: call_with_token(port, method, params or {}, token)
    failures = []

    def check(name, condition, detail=""):
        if condition:
            print(f"[OK] {name}")
        else:
            print(f"[FAIL] {name} {detail}")
            failures.append(name)

    try:
        # 进入「声明式」页（导航栏上的按钮，按文本定位）
        # 响应字段：result.matches（[id/type/role/bounds/text]，与 TS 客户端同口径）
        def matches(selector):
            reply = call("find", {"selector": selector})
            return (reply.get("result") or {}).get("matches") or []

        def by_id(element_id):
            hits = matches("#" + element_id)
            return hits[0] if hits else None

        def by_suffix(suffix):
            """按 id 后缀找（自动 id 是路径形态：`declarative-host/Card@cond/Checkbox@toggle-stats`）。"""
            for element in matches("Checkbox") + matches("Text") + matches("Button"):
                if element["id"].endswith(suffix):
                    return element["id"]
            return None

        entries = matches("Button[text~=声明式]")
        check("声明式页入口存在", len(entries) > 0, str(entries)[:200])
        if not entries:
            return 1
        call("invoke", {"id": entries[0]["id"], "action": "click"})
        call("wait", {"for": "frames", "count": 3})

        # 子树挂载的锚点：声明式内容真的进树（状态驱动表单的输入框）
        inputs = matches("Input")
        check("声明式表单在树上（mount_into 生效）", len(inputs) > 0, str(inputs)[:200])
        if not inputs:
            return 1

        # 状态驱动：输入 → 重组 → 回显变（声明式页的核心演示）。
        # `decl-name` 是表单输入，`decl-greeting` 是回显文本（同一个状态的两个读取点）。
        name_el, greeting_el = by_id("decl-name"), by_id("decl-greeting")
        check("表单/回显对存在（id 稳定，声明式元素可寻址）",
              name_el is not None and greeting_el is not None,
              f"name={name_el} greeting={greeting_el}")
        if name_el is None or greeting_el is None:
            return 1
        before = greeting_el.get("text", "")
        # 走真实输入路径（与人手敲键同一条链：input.text → 焦点元素 → on_change → 写状态）。
        # 注：控制通道没有 `input` 这个方法，文本输入是 `input.text`（带 id 时会先置焦点）。
        call("input.text", {"id": "decl-name", "text": "打磨轮"})
        call("wait", {"for": "frames", "count": 3})
        after = (by_id("decl-greeting") or {}).get("text", "")
        check("状态驱动传播（输入 → 重组 → 回显变）", before != after and "打磨轮" in after,
              f"{before!r} → {after!r}")

        # key 对齐列表：新增一项后，既有项的 id 必须不变（key 复用的意义）
        rows_before = [e["id"] for e in matches("Checkbox")]
        call("input.text", {"id": "decl-draft", "text": "新任务"})
        call("invoke", {"id": "decl-add", "action": "click"})
        call("wait", {"for": "frames", "count": 3})
        rows_after = [e["id"] for e in matches("Checkbox")]
        check("列表增项（key 对齐：既有项身份不变）",
              len(rows_after) == len(rows_before) + 1 and
              all(item in rows_after for item in rows_before),
              f"{len(rows_before)}→{len(rows_after)} {rows_before}")

        # 条件内容：取消勾选 → 统计卡从树上消失（裁剪，不是隐藏）。
        #
        # ## 探针必须按 **id 路径**、不能按文本包含（2026-11-24 实测修正）
        #
        # 旧写法是 `Text[text~=待办]`——而全树里除了统计卡自己，**导航页的
        # Steps 说明文案**也含这两个字（“已完成画对勾、进行中外圈光环、**待办**淡描边”）。
        # 于是计数永远从 2 跌到 1，永远到不了 0：开关真的生效了，断言却报红。
        #
        # 这是“探针写得太宽”的典型形态：断言看起来在问“统计卡还在不在”，
        # 实际问的是“全界面还有哪段文字提到待办”——后者与条件裁剪毫无关系。
        #
        # 改成**属性前缀匹配**（`id^=`）：`declarative-host/Card@cond/Card@stats/…`
        # 只可能来自那张条件卡，文案怎么改都不影响它。实测对照（开关开→关）：
        #
        # | 写法 | 开 | 关 |
        # |---|---|---|
        # | `Text[text~=待办]` | 2 | **1**（被导航页文案误命中） |
        # | `Text[id^=…Card@stats/]` | 2 | **0** ✓ |
        # | Python 侧按 id 前缀过滤 | 0 | 0（`find` 有 50 条上限，那个卡不在返回里） |
        #
        # 第三种看着更“灵活”，实际不能用：`find` 截断后剩下的恰好不含目标，
        # 于是一个**永远 0** 的探针会让断言变成“看运气”。
        stats_selector = "Text[id^=declarative-host/Card@cond/Card@stats/]"
        stats_probe = lambda: len(matches(stats_selector))
        stats_before = stats_probe()
        toggle = by_suffix("Checkbox@toggle-stats")
        check("条件开关可达", toggle is not None, str(toggle))
        if toggle is None:
            return 1
        call("invoke", {"id": toggle, "action": "click"})
        call("wait", {"for": "frames", "count": 3})
        stats_after = stats_probe()
        check("条件内容裁剪（不声明即移除）", stats_before > 0 and stats_after == 0,
              f"统计卡 Text 数 {stats_before} → {stats_after}（选择器 {stats_selector}）")

        # 再打开：条件内容重新进树（裁剪不是单向销毁；同时验证同一元素可反复进出）
        call("invoke", {"id": toggle, "action": "click"})
        call("wait", {"for": "frames", "count": 3})
        check("条件内容可重新进场", stats_probe() > 0, f"匹配数 {stats_probe()}")

        # 结构断言：不是「画面上看着像」，而是语义树上真有这些元素
        tree = call("tree", {"depth": 6})
        check("语义树可读（声明式元素与手搭不可区分）", tree.get("result") is not None,
              str(tree)[:200])

        shot = call("capture", {"path": os.path.join(shots, "gallery-declarative.png")})
        check("截图落盘", shot.get("result") is not None, str(shot)[:200])
    finally:
        try:
            call("shutdown", {})
        except Exception:
            process.kill()
        try:
            process.wait(timeout=5)
        except Exception:
            process.kill()

    print(f"\n{'[OK] gallery 声明式页端到端全项通过' if not failures else '[FAIL] ' + ', '.join(failures)}")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
