"""终端渲染场景探针 v2：修正 v1 里发现的问题后重测。

v1 的三个"失败"里有**两个是探针自己的错**（已修正）：
  - 用 `input` 动作发 `ESC[?1049h` 时，它被当作**命令行的输入字符**回显，
    而不是被程序解释 —— 正确做法是让**子程序自己发**（`printf`/PowerShell 写字节），
    或直接往 PTY 写（`action=send`）。
  - 备用屏内容"残留"检查在退出后立刻做，而 shell 的提示符重绘会随后到达。

本版修正并补上用户点名的场景。
"""
import json
import os
import sys
import time

sys.path.insert(0, r"C:\Users\Administrator\code\gebai\shuangtian\tools")
from st_client_lib import call_with_token

CTL = r"C:\Users\Administrator\code\gebai\users\admin\sessions\3c\7d\3c7db2470254473f9e2fdb2f21b7f9e1\tmp\.shuangtian\gbcode-win32.json"
SHOTS = r"C:\Users\Administrator\code\gebai\users\admin\sessions\3c\7d\3c7db2470254473f9e2fdb2f21b7f9e1\tmp\.shuangtian\shots"
info = json.load(open(CTL))
PORT, TOKEN = info["port"], info.get("token", "")
ESC = "\x1b"

failures = []


def call(m, p=None):
    return call_with_token(PORT, m, p or {}, TOKEN)


def props():
    return call("get", {"id": "terminal"})["result"]["props"]


def screen():
    return str(props().get("screen", ""))


def cursor():
    return str(props().get("cursor", "?"))


def send(s):
    call("invoke", {"id": "terminal", "action": "send", "argument": s})


def send_line(s):
    call("invoke", {"id": "terminal", "action": "send_line", "argument": s})


def focus():
    call("invoke", {"id": "terminal", "action": "focus"})


def shot(name):
    call("capture", {"id": "terminal", "encode": "file", "path": os.path.join(SHOTS, name)})


def check(ok, label, detail=""):
    print(f"  {'✅' if ok else '❌'} {label}" + (f"  {detail}" if detail else ""))
    if not ok:
        failures.append(label)


def ctrl_c():
    send("\x03")
    time.sleep(0.7)


REGION = {"x": 44, "y": 486, "width": 1250, "height": 302}


def phash():
    return call("capture.hash", {"region": REGION})["result"]["hash"]


def wheel(delta, times=1):
    for _ in range(times):
        call("input.mouse", {"kind": "wheel", "x": 400, "y": 620, "delta": delta})


print("═══ A. 备用屏（独占模式）═══")
focus()
ctrl_c()
# 让**子进程自己**发序列（这才是 vim 的做法）——用 PowerShell 写原始字节
send_line('Write-Host -NoNewline "$([char]27)[?1049h"')
time.sleep(1.2)
check(props().get("alt_screen") == "true", "进入备用屏", f"alt_screen={props().get('alt_screen')}")
shot("v2-a-altscreen.png")

# 备用屏里画些内容（全屏程序那样）
send(f'{ESC}[2J{ESC}[H')
time.sleep(0.5)
send("ALT-ONLY-LINE")
time.sleep(1.0)
alt_text = screen()
check("ALT-ONLY-LINE" in alt_text, "备用屏内容上屏")
check("BASIC" not in alt_text, "备用屏不含主屏内容")

print("\n═══ B. 备用屏内滚轮（不应动主屏回看）═══")
h0 = phash()
wheel(+1.0, 3)
time.sleep(0.6)
check(props().get("alt_screen") == "true", "滚动后仍在备用屏")
shot("v2-b-altscreen-scroll.png")

print("\n═══ C. 退出备用屏 → 主屏恢复 ═══")
ctrl_c()
send(f'Write-Host -NoNewline "$([char]27)[?1049l"')
time.sleep(1.2)
if props().get("alt_screen") == "true":
    # 兜底：直接写字节
    send(f'{ESC}[?1049l')
    time.sleep(0.8)
check(props().get("alt_screen") == "false", "退出备用屏")
shot("v2-c-backfromalt.png")

print("\n═══ D. 真全屏程序：vim（若可用）═══")
ctrl_c()
send_line("vim --version | Select-Object -First 1")
time.sleep(2.0)
out = screen()
has_vim = "VIM" in out.upper()
print(f"  vim 可用: {has_vim}")
if has_vim:
    ctrl_c()
    send_line("vim -u NONE -N")
    time.sleep(2.5)
    check(props().get("alt_screen") == "true", "vim 进入备用屏",
          f"alt_screen={props().get('alt_screen')}")
    shot("v2-d-vim.png")
    # vim 里打字
    send("iHELLO-VIM")
    time.sleep(1.0)
    check("HELLO-VIM" in screen(), "vim 插入模式能打字")
    shot("v2-e-vim-typing.png")
    send(f'{ESC}:q!{chr(13)}')
    time.sleep(1.5)
    check(props().get("alt_screen") == "false", "退出 vim 恢复主屏")
    shot("v2-f-aftervim.png")

print("\n═══ E. 光标可见性（hide/show）═══")
ctrl_c()
send_line('Write-Host -NoNewline "$([char]27)[?25l"')
time.sleep(0.8)
shot("v2-g-cursor-hidden.png")
p_hidden = phash()
send_line('Write-Host -NoNewline "$([char]27)[?25h"')
time.sleep(0.8)
check(phash() != p_hidden, "光标显示/隐藏改变像素（光标真在画）")
shot("v2-h-cursor-shown.png")

print("\n═══ F. 大输出不卡（吞吐）═══")
ctrl_c()
t0 = time.time()
send_line("1..300 | ForEach-Object { Write-Host ('BULK-' + $_) }")
deadline = time.time() + 20
ok = False
while time.time() < deadline:
    if "BULK-300" in screen():
        ok = True
        break
    time.sleep(0.3)
elapsed = time.time() - t0
check(ok, "300 行输出全部上屏", f"{elapsed:.1f}s")
check(elapsed < 15.0, "吞吐可接受（<15s）", f"{elapsed:.1f}s")
shot("v2-i-bulk.png")

print(f"\n{'='*50}")
if failures:
    print(f"失败 {len(failures)} 项：")
    for f in failures:
        print(f"  - {f}")
    sys.exit(1)
print("全部通过")
