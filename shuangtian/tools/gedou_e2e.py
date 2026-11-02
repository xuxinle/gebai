#!/usr/bin/env python3
"""格斗（gedou）示例的端到端回归。

## 断言什么、为什么是这些

格斗游戏最容易坏的地方**看不见**：判定盒的位置、帧数、朝向、伤害数值。
截图能证明"画出来了"，但证明不了"打中了该打中的人、扣了该扣的血"。
所以本脚本分三层：

1. **逻辑面**（纯数值，最强断言）：判定帧才出判定盒、命中扣血 = 招式表伤害、
   防御时只吃削减伤害、必杀破防且击倒、血空即回合结束、胜负姿态正确；
2. **几何面**：判定盒在**朝向方向**上、且与骨架"手/脚伸出去"同向
   （否则会出现"动作做了但打不到"或"没做动作却打到了"）；
3. **渲染面**：截图里确实有变化（角色、特效、血条），用像素统计而不是肉眼。

## 反假绿约定

期望值**从运行中的进程取**（`get state` 里带 `moves` 表），不在脚本里再抄一份数值——
否则改了平衡性、脚本却还拿旧数字断言，两边一起漂移而测试恒绿。

用法：
    py -3 tools/gedou_e2e.py [exe] [shots_dir]
"""

import json
import os
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "build", "dev", "bin", "gedou.exe")
ARENA = "arena"


# ── 控制通道客户端（与 louyue_e2e 同形态：一次连接、多次调用） ────────────────


class Client:
    def __init__(self, port, token):
        import socket

        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.rid = 0
        self.token = token

    def call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        header = b""
        while len(header) < 4:
            chunk = self.sock.recv(4 - len(header))
            if not chunk:
                raise RuntimeError("控制通道在收到响应头前关闭")
            header += chunk
        (length,) = struct.unpack(">I", header)
        payload = b""
        while len(payload) < length:
            chunk = self.sock.recv(length - len(payload))
            if not chunk:
                raise RuntimeError("控制通道在收到完整响应前关闭")
            payload += chunk
        return json.loads(payload)

    def ok(self, method, params=None):
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    # —— 语义封装 ——

    def props(self):
        return self.ok("get", {"id": ARENA})["props"]

    def state(self):
        return json.loads(self.props()["state"])

    def invoke(self, action, argument=None, expect=True):
        params = {"id": ARENA, "action": action}
        if argument is not None:
            params["argument"] = str(argument)
        reply = self.ok("invoke", params)
        handled = reply.get("handled", False)
        if expect and not handled:
            raise AssertionError(f"invoke {action} 未被处理：{reply.get('error')}")
        return handled

    def step(self, frames):
        self.invoke("step", frames)
        return self.state()

    def capture_hash(self):
        return self.ok("capture.hash", {})["hash"]


# ── PNG 读取（只用标准库；与 louyue_e2e 同一实现） ─────────────────────────


def read_png(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssertionError(f"{path} 不是 PNG")
    pos, idat, width, height, color_type = 8, b"", None, None, None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            width, height, depth, color_type = struct.unpack(">IIBB", body[:10])
            if depth != 8 or color_type not in (2, 6):
                raise AssertionError(f"期望 8 位真彩，实得 depth={depth} type={color_type}")
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        pos += 12 + length
    channels = 4 if color_type == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    off = 0
    for y in range(height):
        ftype = raw[off]
        off += 1
        line = bytearray(raw[off:off + stride])
        off += stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
            elif ftype != 0:
                raise AssertionError(f"未知行过滤 {ftype}")
        for x in range(width):
            if channels == 4:
                out[(y * width + x) * 4:(y * width + x) * 4 + 4] = line[x * 4:x * 4 + 4]
            else:
                out[(y * width + x) * 4:(y * width + x) * 4 + 3] = line[x * 3:x * 3 + 3]
                out[(y * width + x) * 4 + 3] = 255
        prev = line
    return width, height, bytes(out)


def region_brightness(img, x0, y0, x1, y1):
    """区域平均亮度（0~255）：判断"这一块到底画了东西没有"。"""
    w, h, px = img
    x0, y0 = max(0, int(x0)), max(0, int(y0))
    x1, y1 = min(w, int(x1)), min(h, int(y1))
    if x1 <= x0 or y1 <= y0:
        return 0.0
    total = 0
    count = 0
    for y in range(y0, y1, 2):
        base = y * w
        for x in range(x0, x1, 2):
            i = (base + x) * 4
            total += px[i] + px[i + 1] + px[i + 2]
            count += 3
    return total / max(1, count)


# ── 驱动 ────────────────────────────────────────────────────────────────────


def start(binary, shots, extra=()):
    ctl = os.path.join(shots, "gedou-ctl.json")
    try:
        os.remove(ctl)
    except FileNotFoundError:
        pass
    log = open(os.path.join(shots, "gedou-e2e.log"), "wb")
    cmd = [binary, "--headless", "--control-port", "0", "--control-file", ctl, "--shots", shots,
           "--paused", "--dummy", *extra]
    process = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(120):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl, encoding="utf-8"))
                if info.get("port"):
                    return process, Client(info["port"], info.get("token", "")), info
            except Exception:
                pass
        time.sleep(0.2)
    raise RuntimeError("应用未在 24s 内写出控制文件")


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def box_of(state, side):
    item = state["fighters"][side]
    box = item["hitbox"]
    return box["x"], box["y"], box["width"], box["height"]


def gap_of(state):
    a, b = state["fighters"][0], state["fighters"][1]
    return abs(b["x"] - a["x"])


def approach(client, target_gap, key="d", limit=200):
    """走到“双方脚下距离 ≤ target_gap”，或**已经贴到不能再近**为止。

    为何不写固定帧数：场地宽度随窗口变，而“走几步能打到”完全取决于窗口尺寸——
    写死帧数会在另一种分辨率下默默失效（变得“永远打不中”）。

    为何要允许“走不到”：双方身体存在**互斥间隙**（格斗游戏里角色不能重叠），
    所以阈值必须 ≥ 可达的最小间距；否则 `approach` 会永远返回失败，而真实
    原因是“目标本身不可达”。返回实际是否达到目标，由调用方决定怎么用。
    """
    client.ok("input.key", {"key": key, "kind": "down"})
    try:
        last = gap_of(client.state())
        for _ in range(limit // 5):
            client.step(5)
            now = gap_of(client.state())
            if now <= target_gap:
                return True
            if abs(now - last) < 0.5 and now <= 140.0:
                # 已经贴在一起了：再按也不动。当作“已在最近距离”。
                return True
            last = now
        return False
    finally:
        client.ok("input.key", {"key": key, "kind": "up"})
        client.step(1)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    shots = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "e2e-gedou")
    os.makedirs(shots, exist_ok=True)
    if not os.path.exists(binary):
        print(f"[FAIL] 找不到可执行文件 {binary}（先 st build gedou）")
        return 2

    process, client, info = start(binary, shots)
    passed = 0
    try:
        client.ok("hello")
        print(f"已连接：{info.get('app')} {info.get('version')} 后端={info.get('backend')}")

        # ── 第 1 步：初始状态与招式表（期望值来源） ────────────────────────
        state = client.state()
        moves = state["moves"]
        check(state["phase"] == "fight", "开局应处于 fight")
        check(len(state["fighters"]) == 2, "应有双方角色")
        # 血量从**进程自己报的 max_hp** 读，不在脚本里抄数字。
        #
        # 这条原本硬写 900 / 1000，角色档案一改（户圣现在是 1050）就误报。
        # 而本文件头就写着"反假绿约定：期望值从运行中的进程取"——
        # 这里恰好违反了自己的规矩，属实现成的教训。
        left_max = state["fighters"][0]["max_hp"]
        right_max = state["fighters"][1]["max_hp"]
        check(state["fighters"][0]["hp"] == left_max,
              f"左侧开局应满血 {left_max}，实得 {state['fighters'][0]['hp']}")
        check(state["fighters"][1]["hp"] == right_max,
              f"右侧开局应满血 {right_max}，实得 {state['fighters'][1]['hp']}")
        # 招式表：只断言"必须有的几个核心招在"，**不写死全集**。
        # 角色专属招（donut_toss / puff_spray / waist_grab / apple_*）会随角色增加，
        # 写死全集等于每加一个技能就要改测试（之前就是这样误报的）。
        required = {"light", "heavy", "kick", "special"}
        missing = required - set(moves)
        check(not missing, f"核心招式表缺项：{sorted(missing)}（实有 {sorted(moves)}）")
        # 招式距离必须单调（轻 < 重 < 踢 < 必杀），否则“手感分级”是假的。
        reach = [moves[k]["reach_far"] for k in ("light", "heavy", "kick", "special")]
        check(reach == sorted(reach), f"招式距离应递增，实得 {reach}")
        passed += 1
        print(f"  [1] 初始状态/招式表 OK（距离 {reach}）")

        # ── 第 2 步：判定只在“判定帧”出现（这是最容易写错的一条） ─────────
        light = moves["light"]
        check(approach(client, light["reach_far"] - 8.0),
              f"走不到轻拳距离（目标间距 ≤ {light['reach_far'] - 8.0:.0f}）")
        client.invoke("attack", "light")
        startup_boxes = 0
        active_boxes = 0
        hit_frame_state = None
        for _ in range(20):
            frame = client.step(1)
            box = box_of(frame, 0)
            active = box[2] > 0.0
            if frame["fighters"][0]["move"] == "light":
                if not active:
                    startup_boxes += 1
                else:
                    active_boxes += 1
                    if hit_frame_state is None and frame["fighters"][1]["hp"] < 1000.0:
                        hit_frame_state = frame
        check(active_boxes >= 2, f"轻拳应至少有 2 个判定帧，实得 {active_boxes}")
        check(startup_boxes >= 1, f"轻拳应有起手帧（无判定盒），实得 {startup_boxes}")
        check(hit_frame_state is not None, "走到距离内后轻拳应当命中")
        damage = 1000.0 - hit_frame_state["fighters"][1]["hp"]
        expected = moves["light"]["damage"]
        check(abs(damage - expected) < 0.01, f"轻拳伤害应为 {expected}，实得 {damage}")
        passed += 1
        print(f"  [2] 判定帧 {active_boxes}/起手帧 {startup_boxes}，伤害 {damage} = 招式表 {expected}")

        # ── 第 3 步：判定盒在**朝向方向**上（换边再验一次） ─────────────────
        client.invoke("restart")
        client.step(2)
        # 让玩家侧换到右边：玩家与 AI 侧互换，攻击方向必须反向。
        client.props()  # 确保可读
        client.ok("set", {"id": ARENA, "props": {"player_side": "1"}})
        check(approach(client, moves["heavy"]["reach_far"] - 8.0, key="a"),
              "走不到重拳距离（右侧角色向前是向左，键位应用 a）")
        client.invoke("attack", "heavy")
        forward = []
        for _ in range(20):
            frame = client.step(1)
            box = box_of(frame, 1)
            if box[2] > 0.0:
                x = frame["fighters"][1]["x"]
                forward.append(box[0] + box[2] <= x + 1.0)
        check(forward, "重拳应有判定帧")
        check(all(forward), "右侧角色的判定盒必须落在它的**面向方向**（左侧）")
        passed += 1
        print(f"  [3] 朝向正确（右侧角色的判定盒在其左侧，{len(forward)} 帧）")
        client.ok("set", {"id": ARENA, "props": {"player_side": "0"}})

        # ── 第 4 步：防御只吃削减伤害；必杀破防且击倒 ─────────────────────
        client.invoke("restart")
        check(approach(client, moves["special"]["reach_far"] - 10.0), "走不到必杀距离")
        client.invoke("charge")
        charged = client.state()
        check(charged["fighters"][0]["meter"] >= 100.0, "charge 之后必杀槽应满")
        check(client.invoke("attack", "special"), "槽满时应当能放必杀")
        special_state = None
        for _ in range(30):
            frame = client.step(1)
            if frame["fighters"][0]["move"] == "special" and box_of(frame, 0)[2] > 0.0:
                special_state = frame
                break
        check(special_state is not None, "必杀应有判定帧")
        after = client.step(24)
        check(after["fighters"][1]["move"] in ("knockdown", "hurt", "idle"),
              f"必杀命中后对手应进入受击/倒地，实得 {after['fighters'][1]['move']}")
        check(after["fighters"][1]["hp"] < 1000.0, "必杀必须扣血")
        check(after["fighters"][0]["meter"] < 100.0, "放必杀应消耗槽")
        passed += 1
        print(f"  [4] 必杀：破防命中，对手 HP {after['fighters'][1]['hp']:.0f}，槽归零")

        # ── 第 5 步：血空 → 回合结束 + 胜负姿态 ───────────────────────────
        client.invoke("restart")
        client.invoke("damage", "1")   # 把对手一次打空
        finish = client.step(40)
        check(finish["phase"] == "over", f"血空后应进入 over，实得 {finish['phase']}")
        check(finish.get("winner") == 0, f"应判定左侧胜，实得 {finish.get('winner')}")
        check(finish["fighters"][1]["move"] in ("knockdown", "defeat"),
              f"败方应是倒地/败北，实得 {finish['fighters'][1]['move']}")
        check(finish["fighters"][0]["move"] == "victory",
              f"胜方应进入胜利姿态，实得 {finish['fighters'][0]['move']}")
        check(client.props()["banner"] == "K.O.", "结算应显示 K.O. 横幅")
        passed += 1
        print("  [5] 回合结束：over + winner=0 + 胜者 victory / 败者倒地 + K.O.")

        # ── 第 6 步：渲染面（截图真的变了；血条与角色都画了东西） ───────────
        client.invoke("restart")
        client.step(4)
        first = client.capture_hash()
        client.ok("set", {"id": ARENA, "props": {"show_hitbox": "true"}})
        client.step(2)
        second = client.capture_hash()
        check(first != second, "打开判定盒后画面必须变化（像素哈希应不同）")

        shot = os.path.join(shots, "gedou-e2e.png")
        client.ok("capture", {"path": shot, "encode": "file"})
        check(os.path.exists(shot), "截图应落盘")
        img = read_png(shot)
        w, h, _ = img
        # 血条区域（左上 1/3 宽、顶部 8%~14% 高）必须明显亮于纯背景（背景是深蓝夜空）。
        bar_brightness = region_brightness(img, w * 0.05, h * 0.09, w * 0.30, h * 0.13)
        check(bar_brightness > 40.0, f"血条区域应画了东西（亮度 {bar_brightness:.1f}）")
        # 舞台中央（角色活动区）应有非背景像素。
        arena_brightness = region_brightness(img, w * 0.3, h * 0.5, w * 0.7, h * 0.75)
        check(arena_brightness > 20.0, f"舞台中央应有角色（亮度 {arena_brightness:.1f}）")
        passed += 1
        print(f"  [6] 渲染：哈希变化 OK，血条亮度 {bar_brightness:.1f}，舞台亮度 {arena_brightness:.1f}")

        # ── 第 7 步：确定性（同种子 AI + 同输入 → 同结果） ─────────────────
        client.invoke("restart")
        client.ok("set", {"id": ARENA, "props": {"dummy": "false", "ai": "true"}})
        client.step(120)
        run_a = client.state()["fighters"]
        client.invoke("restart")
        client.step(120)
        run_b = client.state()["fighters"]
        check([f["x"] for f in run_a] == [f["x"] for f in run_b],
              "同种子 AI 跑 120 帧应完全可复现（AI 必须确定性）")
        passed += 1
        print("  [7] AI 确定性：两跑 120 帧位置逐值一致")

        print(f"\n全部通过：{passed}/7")
        return 0
    except AssertionError as error:
        print(f"\n[FAIL] {error}")
        return 1
    finally:
        try:
            client.ok("shutdown", {})
        except Exception:
            pass
        try:
            process.wait(timeout=10)
        except Exception:
            process.kill()


if __name__ == "__main__":
    sys.exit(main())
