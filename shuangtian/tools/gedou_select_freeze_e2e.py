#!/usr/bin/env python3
"""验证"选人界面期间逻辑不推进"（防止"还没开始就在打"）。

## 这条回归为什么必须单独跑

`tools/gedou_e2e.py` 用 `--paused` 启动，本来就冻结逻辑——
所以它**测不到**这个问题。而缺陷恰恰是"没暂停时，选人期间战斗在跑"：

- 用户打开应用 → 停在选人界面思考 30 秒 → 点开始
- 结果开场时双方已经掉了血、连击数在走、计时器跑了一截

根因：`paint_content()` 推逻辑帧时只判了 `view_.paused`，没判 `select_.active`。

## 怎么测

1. **不暂停**启动（正常桌面启动方式）；
2. 等 N 秒，反复读 `logic_frames` —— 必须**完全不变**；
3. 点开始（控制通道 `invoke start_select`），再等，`logic_frames` 应当开始增长。

判据用 `logic_frames` 而不是血量：它是逻辑推进的直接计数器，
血量可能因为"AI 没出手"而暂时不变，会给出假绿。
"""
import json
import os
import subprocess
import sys
import time

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "build", "dev", "bin", "gedou.exe")

sys.path.insert(0, os.path.join(ROOT, "tools"))
from st_client_lib import call_with_token  # noqa: E402


def start(binary, shots):
    ctl = os.path.join(shots, "select-ctl.json")
    try:
        os.remove(ctl)
    except FileNotFoundError:
        pass
    os.makedirs(shots, exist_ok=True)
    log = open(os.path.join(shots, "select-freeze.log"), "wb")
    # ⚠ 关键：**不带 --paused**（带了就测不出问题），也不带 --ai-vs-ai
    # （要停在选人界面观察）。
    cmd = [binary, "--headless", "--control-port", "0", "--control-file", ctl, "--shots", shots]
    process = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(120):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl, encoding="utf-8"))
                if info.get("port"):
                    return process, info["port"], info.get("token", "")
            except Exception:
                pass
        time.sleep(0.2)
    raise RuntimeError("应用未在 24s 内写出控制文件")


def get_prop(port, token, name):
    """读 `#arena` 的某个属性。

    ⚠ 响应结构是**三层**：`{id, ok, result: {props: {...}}}`。
    早先直接读 `response["props"]` 永远拿到 None，于是"启动后不在选人界面"
    这个 SKIP 分支把整条用例跳过了——**看起来通过、实际什么都没测**。
    这类"读错层级导致恒绿"的坑比断言写错更难发现。
    """
    response = call_with_token(port, "get", {"id": "arena"}, token)
    if not isinstance(response, dict):
        return None
    if response.get("ok") is False:
        return None
    result = response.get("result")
    if isinstance(result, dict):
        props = result.get("props")
        if isinstance(props, dict):
            return props.get(name)
    props = response.get("props")
    if isinstance(props, dict):
        return props.get(name)
    return None


def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    shots = os.path.join(ROOT, "build", "e2e-select")
    if not os.path.exists(binary):
        print(f"[FAIL] 找不到 {binary}（先 st build gedou）")
        return 2

    process, port, token = start(binary, shots)
    failures = []
    try:
        phase = get_prop(port, token, "phase")
        print(f"启动后 phase = {phase}")
        if phase != "select":
            print(f"[SKIP] 启动后不在选人界面（phase={phase}），本用例不适用")
            return 0

        # ① 选人期间：等 4 秒，logic_frames 必须一动不动
        first = int(get_prop(port, token, "logic_frames") or 0)
        time.sleep(4.0)
        second = int(get_prop(port, token, "logic_frames") or 0)
        if second != first:
            failures.append(f"选人期间逻辑仍在推进：logic_frames {first} -> {second}")
        else:
            print(f"  [1] 选人期间 frozen OK（logic_frames 稳定在 {first}）")

        # ② 血量也必须没动
        state = json.loads(get_prop(port, token, "state") or "{}")
        hp = [f.get("hp") for f in state.get("fighters", [])]
        if len(hp) == 2:
            max_hp = [f.get("max_hp") for f in state.get("fighters", [])]
            if hp != max_hp:
                failures.append(f"选人期间血量已变化：{hp}（满血应为 {max_hp}）")
            else:
                print(f"  [2] 双方满血 OK（{hp}）")

        # ③ 点开始 → 逻辑应当开始推进
        # 切成机机模式（点模式开关），然后才开始——这样第 ④~⑥ 条才测得到。
        call_with_token(port, "invoke", {"id": "arena", "action": "select_hit", "argument": "2"}, token)
        mode_state = json.loads(get_prop(port, token, "state") or "{}")
        print(f"  模式已切到：{mode_state.get('select', {}).get('mode', '?')}")

        started = call_with_token(port, "invoke",
                                  {"id": "arena", "action": "start_select", "argument": ""}, token)
        # 响应是 {id, ok, result:{handled:bool}}；`ok` 为假说明方法/目标不对。
        handled = True
        if isinstance(started, dict):
            if started.get("ok") is False:
                handled = False
            else:
                result = started.get("result")
                if isinstance(result, dict) and "handled" in result:
                    handled = bool(result["handled"])
        if not handled:
            failures.append(f"invoke start_select 未被处理：{started}")
        time.sleep(1.2)
        third = int(get_prop(port, token, "logic_frames") or 0)
        if third <= second:
            failures.append(f"点开始后逻辑没有推进：{second} -> {third}")
        else:
            print(f"  [3] 开始后逻辑推进 OK（{second} -> {third}）")
        # ④ **机机对战要真的打起来**：两侧都得动、都得掉血。
        #
        # 这条为"机机对战不生效"而写：`set_battle_mode` 早先只设 AI 开关
        # 与 `select_.mode`，而左侧用的是 `player_intent_`（只由键盘写入），
        # 于是机机模式下左边那个角色**站着不动**——看起来就是"模式没生效"。
        started_left = json.loads(get_prop(port, token, "state") or "{}")
        left_x0 = (started_left.get("fighters") or [{}])[0].get("x")
        left_moves = set()
        left_damaged = False
        # 采样窗口要够长：AI 有反应计时与攻击冷却，6 秒可能只放 1 个招。
        # 实测 6 秒窗口下左侧只采到 1 种主动技，会把"修好了"误判成红。
        for _ in range(40):
            time.sleep(0.35)
            snapshot = json.loads(get_prop(port, token, "state") or "{}")
            fighters = snapshot.get("fighters") or []
            if len(fighters) < 2:
                continue
            left_moves.add(fighters[0].get("move"))
            if float(fighters[0].get("hp", 0)) < float(fighters[0].get("max_hp", 1)):
                left_damaged = True
        left_x1 = (json.loads(get_prop(port, token, "state") or "{}").get("fighters") or [{}])[0].get("x")

        # ⚠ 判据必须区分**主动出招**与**被打的反应**。
        #
        # 第一版只断言"左侧出现过 >1 种招式 且 位置有变化"——回退验证时它**没变红**：
        # 左侧不被 AI 驱动，但被打中会进 `hurt`/`knockdown`、被击退也会移位，
        # 于是"在动、有招式"同样成立。回退版实测左侧只有 {idle, knockdown}，
        # 全是被动状态。这就是典型的假绿：判据看着合理，实际拦不住缺陷。
        #
        # 正确判据：左侧必须**主动**放攻击招（不含 idle/walk/guard/hurt/knockdown）。
        passive = {"idle", "walk", "guard", "hurt", "knockdown", "defeat", "victory"}
        left_attacks = {move for move in left_moves if move not in passive}
        if len(left_attacks) < 1:
            failures.append(
                f"机机模式下左侧没有主动出招（招式集合 {sorted(left_moves)}，"
                f"主动技只有 {sorted(left_attacks)}）——它没被 AI 驱动")
        else:
            print(f"  [4] 机机模式左侧主动出招 OK（{len(left_attacks)} 种：{sorted(left_attacks)}）")

        # 位置变化单独保留作参考，但**不作为判据**：被击退也会移位。
        if left_x0 is not None and left_x1 is not None:
            print(f"  [5] 左侧位移 {left_x0:.0f} -> {left_x1:.0f}（仅供参考，击退也会产生位移）")

        if not left_damaged:
            failures.append("机机模式下左侧始终满血——两边根本没打起来")
        else:
            print("  [6] 左侧被打到掉血 OK")

    finally:
        try:
            process.terminate()
            process.wait(timeout=6)
        except Exception:
            process.kill()

    if failures:
        for message in failures:
            print(f"[FAIL] {message}")
        return 1
    print("\n全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
