"""稳态下抓 `WHOFULL` / `USEFULL`：首次绘制之后，交互还要求整帧的到底是谁。

与 `partial_repaint_probe.py` 的区别：那个只报 metrics，本脚本**保留应用的 stderr**
并在交互前后分别统计，从而区分「启动期（首帧前标脏，属正常）」与
「稳态交互（该走局部却没走）」。

用法：
    py -3 tools/partial_whofull_probe.py
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "whofull"
sys.path.insert(0, str(ROOT / "tools"))
import st_client_lib  # noqa: E402


def main() -> int:
    exe = ROOT / "build" / "dev" / "bin" / "gbcode.exe"
    if not exe.exists():
        print("跳过：gbcode.exe 不存在")
        return 2
    WORK.mkdir(parents=True, exist_ok=True)
    control = WORK / "control.json"
    control.unlink(missing_ok=True)
    error_log = WORK / "stderr.txt"
    error_log.unlink(missing_ok=True)

    env = {**os.environ, "ST_PARTIAL_DEBUG": "1", "ST_DAMAGE_DEBUG": "1"}
    with error_log.open("wb") as err:
        process = subprocess.Popen(
            [str(exe), "--headless", "--control-port", "0", "--control-file", str(control)],
            cwd=str(exe.parent),
            stdout=subprocess.DEVNULL,
            stderr=err,
            env=env,
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

            def call(method: str, params: dict | None = None) -> dict:
                return st_client_lib.call_with_token(port, method, params or {}, token,
                                                     timeout=20)["result"]

            time.sleep(2.5)
            mark = error_log.stat().st_size if error_log.exists() else 0
            print(f"启动期已产生 {mark} 字节 stderr（首帧前的标脏属正常）")

            m = call("metrics")
            print(f"视口 {m['physical_width']}x{m['physical_height']} · "
                  f"nodes={m['nodes']} · painted={m.get('painted_elements')}")
            print()

            # 稳态交互：改按钮文字
            deltas: list[tuple[str, int]] = []
            for label, props in [("改文字", {"text": "X"}), ("还原", {"text": "按钮"})]:
                before = error_log.stat().st_size
                call("set", {"id": "activity-explorer", "props": props})
                time.sleep(0.4)
                after = error_log.stat().st_size
                metric = call("metrics")
                deltas.append((label, after - before))
                dirty = metric.get("dirty_rect") or {}
                print(f"{label:<10} partial={metric.get('partial_frame')} "
                      f"脏区={dirty.get('width')}x{dirty.get('height')} "
                      f"paint={metric.get('paint_ms', 0):.2f}ms "
                      f"painted={metric.get('painted_elements')} "
                      f"新增 stderr={after - before}B")

            print()
            # 读稳态之后的 WHOFULL / USEFULL
            if error_log.exists():
                text = error_log.read_text(encoding="utf-8", errors="replace")
                tail = text[mark:] if mark else text
                who = re.findall(r"WHOFULL[^\n]*", tail)
                use = re.findall(r"USEFULL[^\n]*", tail)
                print(f"稳态期 WHOFULL {len(who)} 条：")
                for line in who[:12]:
                    print(f"  {line}")
                print(f"稳态期 USEFULL {len(use)} 条：")
                for line in use[:6]:
                    print(f"  {line}")
                if not who and not use:
                    print("  （无——稳态已经没有元素要求整帧了）")
            return 0
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == "__main__":
    sys.exit(main())
