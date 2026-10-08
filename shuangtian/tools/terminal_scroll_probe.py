"""端到端验证（v4，最终）：用 `scroll` 属性面读回看偏移——与被测机制同构。

前两版都错在**量尺**上，记在这里免得下次再踩：
  v1 用 `capture.hash` 判"画面变没变" → 被**光标闪烁**污染（闪烁本身就让哈希变）；
  v2 读 `screen` 属性 → 那是**屏幕模型**，而回看是画在屏幕**上方**的叠加层，读不到；
  v3 自以为"先隐藏光标就不闪了"，但**采样间隔 1.5s 恰好是闪烁周期 1s 的整数倍**
     → 永远同相，判据看着稳定其实是被混叠骗了。
本版改用 `scroll` 属性（`偏移:总行数`）：它直接就是被测的那个量。
"""
import json
import sys
import time

sys.path.insert(0, r"C:\Users\Administrator\code\gebai\shuangtian\tools")
from st_client_lib import call_with_token

CTL = r"C:\Users\Administrator\code\gebai\users\admin\sessions\3c\7d\3c7db2470254473f9e2fdb2f21b7f9e1\tmp\.shuangtian\gbcode-win32.json"
info = json.load(open(CTL))
PORT, TOKEN = info["port"], info.get("token", "")
fails = []


def call(m, p=None):
    return call_with_token(PORT, m, p or {}, TOKEN)


def props():
    return call("get", {"id": "terminal"})["result"]["props"]


def screen():
    return str(props().get("screen", ""))


def scroll():
    """返回 (偏移, 总行数)；总行数为 0 时偏移也应为 0。"""
    raw = str(props().get("scroll", ""))
    if ":" not in raw:
        return (-1, -1)
    a, b = raw.split(":", 1)
    return (int(a), int(b))


def wheel(delta, times=1):
    for _ in range(times):
        call("input.mouse", {"kind": "wheel", "x": 400, "y": 620, "delta": delta})
        time.sleep(0.1)
    time.sleep(0.5)


def check(ok, label, detail=""):
    print(f"  {'✅' if ok else '❌'} {label}" + (f"  {detail}" if detail else ""))
    if not ok:
        fails.append(label)


call("invoke", {"id": "terminal", "action": "focus"})
call("invoke", {"id": "terminal", "action": "send", "argument": "\x03"})
time.sleep(0.8)

print("=== 造 80 行历史 ===")
call("invoke", {"id": "terminal", "action": "send_line",
                "argument": "1..80 | ForEach-Object { Write-Host ('SCROLL-' + $_) }"})
time.sleep(4.0)
check("SCROLL-80" in screen(), "长输出已上屏")
off0, total = scroll()
print(f"  初始 scroll = {off0}:{total}")
check(off0 == 0, "初始贴底（偏移 0）")
check(total > 60, "回看缓冲有足够历史", f"总 {total} 行")

print("\n=== 向上滚 5 格 ===")
wheel(1.0, 5)
up, _ = scroll()
check(up > off0, "**向上滚偏移增大**", f"{off0} → {up}")

print("\n=== 向下滚 3 格（本轮缺陷判据）===")
wheel(-1.0, 3)
down, _ = scroll()
check(down < up, "**向下滚偏移减小**（缺陷时此处必红：偏移只会增）", f"{up} → {down}")

print("\n=== 向下滚回底 ===")
wheel(-1.0, 40)
back, _ = scroll()
check(back == 0, "回到偏移 0（贴底）", f"{down} → {back}")

print("\n=== 到底后继续向下：应停住 ===")
wheel(-1.0, 5)
still, _ = scroll()
check(still == 0, "到底后不再变化", f"={still}")

print("\n=== 向上顶到头：应停在缓冲上限 ===")
wheel(1.0, 200)
top, _ = scroll()
check(top == total, "顶到头 = 缓冲总行数", f"{top} vs {total}")
wheel(1.0, 5)
top2, _ = scroll()
check(top2 == top, "顶到头后继续向上不动", f"{top} → {top2}")

print("\n=== 精确步长：一格 = 3 行 ===")
wheel(-1.0, 500)   # 先回底
check(scroll()[0] == 0, "先回到贴底")
wheel(1.0, 1)
one, _ = scroll()
check(one == 3, "向上滚一格 = 3 行", f"偏移 = {one}")
wheel(-1.0, 1)
zero, _ = scroll()
check(zero == 0, "向下滚一格回到贴底", f"偏移 = {zero}")

print("\n=== 滚轮方向与系统一致（向上为正）===")
check(one > 0, "delta=+1（向上）产生正偏移")

print("\n" + "=" * 50)
if fails:
    print(f"失败 {len(fails)} 项：")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("全部通过：双向可滚、两端停住、步长正确、方向与系统一致")
