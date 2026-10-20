"""增量重绘（脏区）现状测量：每次交互是否真的走局部路径。

BACKLOG 的 P0「增量重绘恒退化为整帧」记录：任何交互都 `partial_frame=false`、
`dirty_rect` 恒为全视口。之后 `layout()` 加了「几何签名」优化（重排后矩形不变
就不整帧）。本脚本量**当前**状态，判断 P0 还有多少没修。

用法：
    py -3 tools/partial_repaint_probe.py            # gbcode
    py -3 tools/partial_repaint_probe.py gallery    # 指定示例
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "partial-probe"
sys.path.insert(0, str(ROOT / "tools"))
import st_client_lib  # noqa: E402


def main() -> int:
    app = sys.argv[1] if len(sys.argv) > 1 else "gbcode"
    exe = ROOT / "build" / "dev" / "bin" / f"{app}.exe"
    if not exe.exists():
        print(f"跳过：{exe} 不存在（先 st build {app} --profile dev）")
        return 2

    WORK.mkdir(parents=True, exist_ok=True)
    control = WORK / f"control-{app}.json"
    control.unlink(missing_ok=True)
    process = subprocess.Popen(
        [str(exe), "--headless", "--control-port", "0", "--control-file", str(control)],
        cwd=str(exe.parent),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
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
            return 1
        port, token = int(info["port"]), info["token"]

        def call(method: str, params: dict | None = None) -> dict:
            return st_client_lib.call_with_token(port, method, params or {}, token, timeout=20)[
                "result"
            ]

        time.sleep(2.0)  # 等首帧稳定
        base = call("metrics")
        print(f"视口 {base['physical_width']}x{base['physical_height']} · "
              f"paint={base['paint_ms']:.2f}ms · nodes={base['nodes']}")
        print()

        # 找可交互的控件：按钮 / 输入框 / 勾选框
        tree = call("tree", {"depth": 0})["tree"]
        buttons: list[str] = []
        inputs: list[str] = []

        def walk(node: dict) -> None:
            kind = node.get("type")
            node_id = node.get("id")
            if node_id:
                if kind == "Button" and len(buttons) < 6:
                    buttons.append(node_id)
                elif kind in ("Input", "TextArea", "Checkbox") and len(inputs) < 4:
                    inputs.append(node_id)
            for child in node.get("children") or []:
                walk(child)

        walk(tree)
        print(f"候选按钮 {len(buttons)} 个：{buttons[:6]}")
        print(f"候选输入/勾选 {len(inputs)} 个：{inputs[:4]}")
        print()

        print(f"{'操作':<28} {'partial':<8} {'脏区':<20} {'paint_ms':<10} {'painted'}")
        print("-" * 84)

        def report(label: str) -> None:
            m = call("metrics")
            dirty = m.get("dirty_rect") or {}
            area = f"{dirty.get('width')}x{dirty.get('height')}"
            print(f"{label:<28} {str(m.get('partial_frame')):<8} {area:<20} "
                  f"{m.get('paint_ms', 0):<10.2f} {m.get('painted_elements')}")

        for node_id in buttons[:4]:
            # 改文字（几何可能变，也可能不变）
            call("set", {"id": node_id, "props": {"text": "X"}})
            time.sleep(0.3)
            report(f"set text #{node_id[:18]}")
            call("set", {"id": node_id, "props": {"text": "按钮"}})
            time.sleep(0.3)
            report(f"restore #{node_id[:18]}")

        for node_id in inputs[:3]:
            call("set", {"id": node_id, "props": {"text": "abc"}})
            time.sleep(0.3)
            report(f"set input #{node_id[:18]}")

        # invoke 一个按钮（点击）
        if buttons:
            call("invoke", {"id": buttons[0], "action": "click"})
            time.sleep(0.4)
            report(f"click #{buttons[0][:18]}")

        return 0
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()


if __name__ == "__main__":
    sys.exit(main())
