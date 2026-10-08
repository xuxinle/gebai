"""用真 TUI 替身验「独占模式」全流程（vim/htop 走的就是这条）。

为什么不用 vim：本机**没装 vim**。
（上一版探针报 "vim 可用: True" 是**判据错**——`vim: 术语 'vim' 不会被识别`
这句报错本身含 "VIM"，被 `in out.upper()` 匹配上了。教训见 CONVENTIONS §7.8。）

替身 `tmp_fake_tui.py` 发的是与 vim 同款的 ESC 序列：
  ENTER `ESC[?1049h` → 逐帧整屏重绘 → 读按键 → LEAVE `ESC[?1049l`。
"""
import json
import os
import sys
import time

sys.path.insert(0, r"C:\Users\Administrator\code\gebai\shuangtian\tools")
from st_client_lib import call_with_token

CTL = r"C:\Users\Administrator\code\gebai\users\admin\sessions\3c\7d\3c7db2470254473f9e2fdb2f21b7f9e1\tmp\.shuangtian\gbcode-win32.json"
SHOTS = r"C:\Users\Administrator\code\gebai\users\admin\sessions\3c\7d\3c7db2470254473f9e2fdb2f21b7f9e1\tmp\.shuangtian\shots"
TUI = r"C:\Users\Administrator\code\gebai\shuangtian\tmp_fake_tui.py"
PY = r"C:\Users\Administrator\AppData\Local\Programs\Python\Python312\python.exe"
info = json.load(open(CTL))
PORT, TOKEN = info["port"], info.get("token", "")

fails = []


def call(m, p=None):
    return call_with_token(PORT, m, p or {}, TOKEN)


def props():
    return call("get", {"id": "terminal"})["result"]["props"]


def screen():
    return str(props().get("screen", ""))


def alt():
    return props().get("alt_screen")


def send(s):
    call("invoke", {"id": "terminal", "action": "send", "argument": s})


def send_line(s):
    call("invoke", {"id": "terminal", "action": "send_line", "argument": s})


def shot(n):
    call("capture", {"id": "terminal", "encode": "file", "path": os.path.join(SHOTS, n)})


def check(ok, label, detail=""):
    print(f"  {'✅' if ok else '❌'} {label}" + (f"  {detail}" if detail else ""))
    if not ok:
        fails.append(label)


def wait_for(pred, timeout=8.0, step=0.2):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred():
            return True
        time.sleep(step)
    return False


call("invoke", {"id": "terminal", "action": "focus"})
# 确保 shell 在前台
for _ in range(6):
    send("\x03")
    time.sleep(0.4)
    send_line("echo __R__")
    time.sleep(0.9)
    if "__R__" in screen():
        break
check(alt() == "false", "起始在主屏")

print("\n=== 启动 TUI（进入独占模式）===")
send_line(f'& "{PY}" "{TUI}"')
check(wait_for(lambda: alt() == "true", 10), "TUI 进入备用屏", f"alt={alt()}")
check(wait_for(lambda: "TUI-FRAME-" in screen(), 6), "TUI 帧内容上屏")
shot("tui-1-entered.png")

print("\n=== 独占模式下逐帧重绘 ===")
# 取两帧看内容在变（说明重绘持续生效，不是卡在首帧）
f1 = screen()
time.sleep(1.5)
f2 = screen()
check(f1 != f2, "画面在逐帧刷新（内容随帧号变化）")
shot("tui-2-drawing.png")

print("\n=== 独占模式下的按键响应 ===")
send("x")
time.sleep(0.8)
send("y")
time.sleep(0.8)
check("keys: xy" in screen(), "TUI 收到按键并回显", f"keys 行: {[l for l in screen().split(chr(10)) if 'keys' in l]}")

print("\n=== 反显块（测样式渲染）===")
check("REVERSE-BLOCK" in screen(), "反显内容在屏")
shot("tui-3-reverse.png")

print("\n=== 独占模式内滚轮（不该翻主屏历史）===")
for _ in range(3):
    call("input.mouse", {"kind": "wheel", "x": 400, "y": 620, "delta": 1.0})
time.sleep(0.6)
check(alt() == "true", "滚动后仍在独占模式")
shot("tui-4-scrolled.png")

print("\n=== 退出 TUI（按 q）→ 主屏恢复 ===")
send("q")
check(wait_for(lambda: alt() == "false", 8), "退出后离开备用屏", f"alt={alt()}")
check(wait_for(lambda: "TUI-EXITED-CLEANLY" in screen(), 5), "确认退出信息")
check("TUI-FRAME-" not in screen(), "独占屏内容不残留在主屏")
shot("tui-5-exited.png")

print("\n=== 退出后 shell 可用 ===")
send_line("echo AFTER-TUI-OK")
check(wait_for(lambda: "AFTER-TUI-OK" in screen(), 6), "shell 恢复可用")

print("\n" + "=" * 50)
if fails:
    print(f"失败 {len(fails)} 项：")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("全部通过")
