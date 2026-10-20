"""局部重绘的**像素完整性**校验：局部帧结果必须与强制整帧逐像素一致。

这是 BACKLOG P0「增量重绘恒退化为整帧」里那个卡住后续工作的判据：
加上「零几何元素不要求整帧」后局部路径终于跑起来，但像素比对发现
41,908 个像素差、最大通道差 132（差异全在脏区内，像"旧文字没被清掉"）。

做法：
  ① 触发一次变更 → 让应用走**局部**帧 → 截图 A；
  ② 用 `repaint_full` 类动作（或改主题再改回）强制一次**整帧** → 截图 B；
  ③ 逐像素比对 A / B。局部帧与整帧在同一内容下必须完全一致。

用法：
    py -3 tools/partial_repaint_verify.py
退出码：0 = 一致；1 = 有差异；2 = 环境不具备。
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "partial-verify"
sys.path.insert(0, str(ROOT / "tools"))
import st_client_lib  # noqa: E402


def call(port: int, token: str, method: str, params: dict | None = None) -> dict:
    return st_client_lib.call_with_token(port, method, params or {}, token, timeout=25)["result"]


def capture(port: int, token: str, path: pathlib.Path) -> bool:
    result = call(port, token, "capture", {"encode": "file", "path": str(path)})
    return pathlib.Path(result.get("path", "")).exists()


def diff_stats(left: pathlib.Path, right: pathlib.Path) -> tuple[int, int, list[int]]:
    """返回 (差异像素数, 最大通道差, 差异像素的 y 分布样本)。"""
    try:
        from PIL import Image
    except ImportError:
        print("需要 Pillow：pip install pillow")
        return -1, -1, []
    a = Image.open(left).convert("RGB")
    b = Image.open(right).convert("RGB")
    if a.size != b.size:
        print(f"尺寸不同：{a.size} vs {b.size}")
        return -1, -1, []
    pa, pb = a.load(), b.load()
    count = 0
    worst = 0
    ys: list[int] = []
    for y in range(a.size[1]):
        for x in range(a.size[0]):
            ca, cb = pa[x, y], pb[x, y]
            delta = max(abs(ca[0] - cb[0]), abs(ca[1] - cb[1]), abs(ca[2] - cb[2]))
            if delta > 0:
                count += 1
                worst = max(worst, delta)
                if len(ys) < 40:
                    ys.append(y)
    return count, worst, ys


def main() -> int:
    exe = ROOT / "build" / "dev" / "bin" / "gbcode.exe"
    if not exe.exists():
        print("跳过：gbcode.exe 不存在")
        return 2
    WORK.mkdir(parents=True, exist_ok=True)
    control = WORK / "control.json"
    control.unlink(missing_ok=True)
    env = {"ST_DAMAGE_SKIP_EMPTY": "1"}
    import os

    merged_env = {**os.environ, **env}
    process = subprocess.Popen(
        [str(exe), "--headless", "--control-port", "0", "--control-file", str(control)],
        cwd=str(exe.parent),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=merged_env,
    )
    try:
        deadline = time.time() + 60
        info = {}
        while time.time() < deadline:
            if control.exists():
                try:
                    info = json.loads(control.read_text(encoding="utf-8"))
                    break
                except (json.JSONDecodeError, OSError):
                    pass
            time.sleep(0.3)
        if not info:
            print("失败：控制通道未就绪")
            return 2
        port, token = int(info["port"]), info["token"]
        time.sleep(2.5)

        # 找一个按钮，做一次会让几何**不变**的文字变更（最能体现局部重绘）。
        tree = call(port, token, "tree", {"depth": 0})["tree"]
        buttons: list[str] = []

        def walk(node: dict) -> None:
            if node.get("type") == "Button" and node.get("id") and len(buttons) < 4:
                buttons.append(node["id"])
            for child in node.get("children") or []:
                walk(child)

        walk(tree)
        if not buttons:
            print("跳过：找不到按钮")
            return 2
        target = buttons[0]
        print(f"目标控件 #{target}")

        # ① 先做一次变更，走（可能的）局部帧
        call(port, token, "set", {"id": target, "props": {"text": "X"}})
        time.sleep(0.5)
        m1 = call(port, token, "metrics")
        partial_path = WORK / "partial.png"
        capture(port, token, partial_path)
        print(f"变更后：partial={m1.get('partial_frame')} "
              f"脏区={m1.get('dirty_rect', {}).get('width')}x"
              f"{m1.get('dirty_rect', {}).get('height')} "
              f"paint={m1.get('paint_ms', 0):.2f}ms painted={m1.get('painted_elements')}")

        # ② 强制整帧：改主题再改回（每次主题变更都会 mark_dirty_all → 整帧）
        call(port, token, "theme", {"mode": "dark"})
        time.sleep(0.6)
        call(port, token, "theme", {"mode": "light"})
        time.sleep(0.6)
        m2 = call(port, token, "metrics")
        full_path = WORK / "full.png"
        capture(port, token, full_path)
        print(f"整帧后：  partial={m2.get('partial_frame')} "
              f"paint={m2.get('paint_ms', 0):.2f}ms painted={m2.get('painted_elements')}")

        count, worst, ys = diff_stats(partial_path, full_path)
        if count < 0:
            return 2
        print()
        print(f"像素差异：{count} 个，最大通道差 {worst}")
        if count:
            print(f"差异集中在 y≈{sorted(set(ys))[:12]}")
            print(f"截图：{partial_path} / {full_path}")
            return 1
        print("一致：局部帧与整帧逐像素相同")
        return 0
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()


if __name__ == "__main__":
    sys.exit(main())
