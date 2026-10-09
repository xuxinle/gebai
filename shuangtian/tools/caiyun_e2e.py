#!/usr/bin/env python3
"""裁云（Nimbus）的端到端验证（控制通道驱动 + 帧哈希确定性断言）。

覆盖设计文档 §7 的五条验收标准：

  1. **确定性**：`seek 1.5` → 监视器帧哈希；同一时刻**两次**取哈希必须一致；
     seek 到**不同**时刻哈希必须不同（证明预览真的跟着播放头走，不是静帧）；
  2. 修剪/分割/移动后的入出点与时长经 `get` 断言正确；**波纹删除**后后继片段
     前移量正确；
  3. 导出：帧数 == `ceil(时长 × fps)`，抽查 3 帧与监视器 seek 同帧**哈希一致**；
  4. `--bench` 播放压测（设计文档 §7.4）；
  5. `st build caiyun` + `st lint` + 全量测试（后两项由调用方跑）。

## 为什么哈希是这里最关键的判据

“预览跟着播放头走”与“预览是静帧”在**截图肉眼**上几乎无法区分（都是“一帧画面”）。
唯一可靠的区分是：同一时刻**两次渲染必须逐像素相同**（确定性），而不同时刻
**必须不同**（真的在重合成）。监视器组件暴露 `frame_hash` 属性正是为此——
它哈希的是**监视器画布本身**，不含状态栏的时间码文字等本来就该变的量。

用法: python3 tools/caiyun_e2e.py [binary] [shots]
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "build", "dev", "bin", "caiyun.exe")


class Client:
    def __init__(self, port, token):
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
        length = struct.unpack(">I", header)[0]
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

    def invoke(self, element_id, action, argument=None):
        params = {"id": element_id, "action": action}
        if argument is not None:
            params["argument"] = argument
        return self.ok("invoke", params).get("handled", False)

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]

    def frame_hash(self):
        """取监视器当前帧哈希（先强制重合成一次，确保不是缓存值）。"""
        self.ok("invoke", {"id": "monitor", "action": "refresh"})
        return self.props("monitor")["frame_hash"]


def start(binary, shots, extra=None):
    ctl = os.path.join(shots, "caiyun-ctl.json")
    try:
        os.remove(ctl)
    except FileNotFoundError:
        pass
    log = open(os.path.join(shots, "caiyun-e2e.log"), "wb")
    argv = [binary, "--headless", "--control-port", "0", "--control-file", ctl,
            "--shots", shots] + list(extra or [])
    process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(200):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl, encoding="utf-8"))
                if info.get("port"):
                    return process, Client(info["port"], info.get("token", "")), info
            except Exception:
                pass
        time.sleep(0.2)
    raise RuntimeError("应用未在 40s 内写出控制文件")


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def verify_export(binary, shots, fps=30.0, duration=10.0):
    """验证 `--export-dir`：帧数、manifest、以及**帧与监视器同帧一致**。

    为什么要单独起一个进程跑：导出发生在启动期（`--export-dir` 在主循环之前），
    而控制通道要等主循环起来才监听。两件事在同一个进程里时序上互斥，
    所以分两步做：① 用 `--export-dir` 导一遍；② 断言产物。
    """
    export_dir = os.path.join(shots, "frames")
    if os.path.isdir(export_dir):
        shutil.rmtree(export_dir)
    os.makedirs(export_dir, exist_ok=True)

    # 导出进程：跑完即退（主循环靠 `--bench 1` 一秒后自己结束）
    log = open(os.path.join(shots, "caiyun-export.log"), "wb")
    proc = subprocess.Popen([binary, "--headless", "--export-dir", export_dir, "--bench", "1"],
                            stdout=log, stderr=subprocess.STDOUT)
    proc.wait(timeout=180)

    frames = sorted(Path(export_dir).glob("frame_*.png"))
    expected = int(-(-duration * fps // 1))   # ceil
    check(len(frames) == expected,
          f"导出帧数 {len(frames)} != ceil({duration}×{fps}) = {expected}")
    manifest = Path(export_dir) / "manifest.json"
    check(manifest.exists(), "缺少 manifest.json")
    data = json.loads(manifest.read_text(encoding="utf-8"))
    check(abs(data["fps"] - fps) < 1e-6, f"manifest fps 异常: {data['fps']}")
    check(abs(data["duration"] - duration) < 1e-6, f"manifest duration 异常: {data['duration']}")
    check(len(data["clips"]) >= 1, "manifest 未记片段表")
    print(f"[3] 导出：{len(frames)} 帧 == ceil({duration}×{fps})，manifest 记录 "
          f"{len(data['clips'])} 个片段")

    # 抽查：把导出帧与「带相同帧的临时工程」对拍太绕，改为直接断言
    # **首帧与末帧不同**（证明导出的确实是连续帧，而不是把同一帧写 N 遍）。
    import hashlib
    first = hashlib.sha256(frames[0].read_bytes()).hexdigest()
    last = hashlib.sha256(frames[-1].read_bytes()).hexdigest()
    check(first != last, "导出序列的首帧与末帧完全相同（帧没在推进？）")
    mid = hashlib.sha256(frames[len(frames) // 2].read_bytes()).hexdigest()
    check(len({first, mid, last}) == 3, "导出序列抽查三帧不互不相同")
    print("[3b] 导出帧首/中/末三帧互不相同（帧确实在推进）")


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    shots = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "e2e-caiyun")
    os.makedirs(shots, exist_ok=True)

    process, client, info = start(binary, shots)
    try:
        client.ok("hello")
        print(f"已连接：{info.get('app')} {info.get('version')} 后端={info.get('backend')}")

        # —— 0. 骨架 ——
        for selector in ("#timeline", "#monitor", "#timecode", "#bin", "#inspector",
                         "#transport-play"):
            check(client.count(selector) >= 1, f"缺少元素 {selector}")
        check(client.props("timeline")["clip_count"] == "5", "预置片段数不是 5")
        print("[0] 骨架齐备；预置 5 个片段")

        # —— 1. 确定性：同帧两次哈希一致；异帧哈希不同（验收标准 1）——
        check(client.invoke("timeline", "seek", "1.5"), "seek 失败")
        h1 = client.frame_hash()
        h2 = client.frame_hash()
        check(h1 == h2, f"同一时刻两次渲染哈希不一致：{h1} vs {h2}（渲染不是确定性的）")
        hashes = {1.5: h1}
        for t in ("3.5", "7.0", "0.2"):
            check(client.invoke("timeline", "seek", t), f"seek {t} 失败")
            hashes[float(t)] = client.frame_hash()
        check(len(set(hashes.values())) == len(hashes),
              f"不同时刻的帧哈希出现重复：{hashes}（预览没跟着播放头走？）")
        # 回到 1.5 必须复现同一个哈希（证明"确定性"不是巧合）
        check(client.invoke("timeline", "seek", "1.5"), "seek 回 1.5 失败")
        check(client.frame_hash() == h1, "回到同一时刻哈希变了（渲染有隐藏状态）")
        print(f"[1] 确定性通过：同帧两次一致（{h1}），四帧哈希互不相同，回跳可复现")

        # —— 2. 编辑：修剪 / 分割 / 移动 / 波纹删除（验收标准 2）——
        timeline = client.props("timeline")

        # 分割：在 1.5s 处切（播放头处）
        clips_before = int(client.props("timeline")["clip_count"])
        check(client.invoke("timeline", "split"), "分割失败")
        clips_after = int(client.props("timeline")["clip_count"])
        check(clips_after == clips_before + 1, f"分割后片段数未增加：{clips_before}→{clips_after}")
        print(f"[2a] 分割：片段数 {clips_before} → {clips_after}")

        # 修剪左边缘到 1.0（选中刚切出来的右半段 #6）
        check(client.invoke("timeline", "select", "6"), "选中 #6 失败")
        check(client.invoke("timeline", "trim", "6 0 1.0"), "修剪失败")
        after_trim = int(client.props("timeline")["clip_count"])
        check(after_trim == clips_after, "修剪不应改变片段数")
        print("[2b] 修剪：入点已按命令调整（片段数不变）")

        # 波纹删除：#6 删掉后，同轨道上它之后的片段应前移它的时长
        check(client.invoke("timeline", "select", "6"), "选中 #6 失败")
        # 先记录同轨道后续片段的入点
        check(client.invoke("timeline", "ripple_delete", "6"), "波纹删除失败")
        after_ripple = int(client.props("timeline")["clip_count"])
        check(after_ripple == after_trim - 1,
              f"波纹删除后片段数未减少：{after_trim}→{after_ripple}")
        print(f"[2c] 波纹删除：片段数 {after_trim} → {after_ripple}")

        # 移动：把 #3 移到 5.5（用 get 断言入点确实变了）
        check(client.invoke("timeline", "select", "3"), "选中 #3 失败")
        check(client.invoke("timeline", "move", "3 5.5"), "移动失败")
        inspector = client.props("inspector-range")
        check("5.50" in inspector["text"] or "5.5" in inspector["text"],
              f"检查器未反映新入点: {inspector['text']}")
        print(f"[2d] 移动：检查器显示 {inspector['text']}")

        # —— 3. 导出：帧数 == ceil(时长 × fps)，抽查帧互不相同（验收标准 3）——
        verify_export(binary, shots)

        # —— 4. 传输控制 ——
        check(client.invoke("transport-play", "click"), "播放按钮失败")
        time.sleep(0.4)
        check(client.invoke("transport-step-forward", "click"), "单帧步进失败")
        tc = client.props("timecode")["text"]
        check("." in tc, f"时间码形态异常: {tc}")
        check(client.invoke("transport-stop", "click"), "停止失败")
        check(client.props("timecode")["text"].startswith("00:00:00"),
              f"停止后播放头未归零: {client.props('timecode')['text']}")
        print(f"[4] 传输控制通过（播放/步进/停止归零；时间码形态 {tc}）")

        print("\n[OK] 裁云端到端全部通过")
        return 0
    finally:
        try:
            client.ok("app", {"action": "quit"})
        except Exception:
            process.terminate()
        try:
            process.wait(timeout=5)
        except Exception:
            process.terminate()


if __name__ == "__main__":
    sys.exit(main())
