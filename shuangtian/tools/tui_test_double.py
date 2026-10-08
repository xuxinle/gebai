"""真 TUI 替身：进备用屏 → 逐帧重绘 → 按键响应 → 退出（vim/htop 同款序列）。

为何需要它：本机**没装 vim**，而"独占模式"（备用屏）必须用真程序验。
本脚本用真 ESC 序列驱动，与 vim 的屏控行为同构：
  进入 `ESC[?1049h` → 清屏定位重绘 → 读按键 → 退出 `ESC[?1049l`。
"""
import sys
import time

ESC = "\x1b"

if sys.platform == "win32":
    import msvcrt

    def read_key(timeout=0.2):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if msvcrt.kbhit():
                return msvcrt.getwch()
            time.sleep(0.02)
        return None
else:
    import select

    def read_key(timeout=0.2):
        ready, _, _ = select.select([sys.stdin], [], [], timeout)
        return sys.stdin.read(1) if ready else None


def out(text):
    sys.stdout.write(text)
    sys.stdout.flush()


# —— 进入备用屏 ——
out(f"{ESC}[?1049h")
out(f"{ESC}[2J{ESC}[H")

frame = 0
pressed = []
try:
    while True:
        # 逐帧重绘（模拟 TUI 的整屏刷新）
        out(f"{ESC}[H")
        out(f"{ESC}[1;1HTUI-FRAME-{frame}")
        out(f"{ESC}[2;1HTUI-ALT-SCREEN-ACTIVE")
        out(f"{ESC}[3;1Hkeys: {''.join(pressed)}")
        out(f"{ESC}[5;1HTUI-FRAME-{frame}")   # 反复写，验证重绘不累积
        # 高亮一块（测背景色/反显）
        out(f"{ESC}[7;1H{ESC}[7mREVERSE-BLOCK{ESC}[0m")
        # 光标定位（测光标跟随）
        out(f"{ESC}[9;{5 + frame % 10}H")
        sys.stdout.flush()
        frame += 1

        key = read_key(0.25)
        if key == "q":
            break
        if key is not None and key not in ("\x00", "\xe0"):
            pressed.append(key if key.isprintable() else "?")
        if frame > 40:
            break
finally:
    # —— 退出备用屏（必须恢复主屏）——
    out(f"{ESC}[?1049l")
    out("TUI-EXITED-CLEANLY\n")
    sys.stdout.flush()
