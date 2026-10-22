"""验证 caiyun 的**画面质量度量**：渲染一段已知节奏的脚本，断言 quality 读数与真值一致。

真值设计（可手算，不靠感觉）：
  镜1 `still`  1.5s 纯色不动          → 该镜静止占比应 ≈ 100%
  镜2 `motion` 1.5s 强运动（大位移）  → 该镜静止占比应 ≈ 0%
  镜3 `white`  1.0s 全白              → mean_luma 应 ≈ 255，进 bright_frames
  镜4 `black`  1.0s 全黑              → mean_luma 应 ≈ 0
全片真值：静止占比约 25–60%（2.5s 静止 / 5s）、过亮占比 >15%、平均亮度居中。

跑法：python tools/caiyun_quality_e2e.py [binary]
"""

import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "build", "dev", "bin", "caiyun.exe")


class Client:
    """控制通道客户端（协议 `st-control/1`：**uint32 大端长度前缀** + JSON 帧）。

    不是 NDJSON——写错的表现是服务端报「帧长度非法（20 亿字节）」后**直接断开**，
    而客户端只看到「响应体前关闭」，与根因相距很远（实测踩过一次）。
    token 只在 `hello` 里传，其余方法不带。
    """

    def __init__(self, port, token):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(180.0)
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
                raise RuntimeError("控制通道在响应头前关闭")
            header += chunk
        length = struct.unpack(">I", header)[0]
        payload = b""
        while len(payload) < length:
            chunk = self.sock.recv(length - len(payload))
            if not chunk:
                raise RuntimeError("控制通道在响应体前关闭")
            payload += chunk
        reply = json.loads(payload.decode("utf-8"))
        if not reply.get("ok"):
            raise AssertionError("%s 失败: %s" % (method, reply.get("error")))
        return reply.get("result") or {}

    def ok(self, method, params=None):
        return self.call(method, params)

    def invoke(self, element_id, action, argument=None):
        p = {"id": element_id, "action": action}
        if argument is not None:
            p["argument"] = argument
        return self.ok("invoke", p).get("handled", False)

    def props(self, element_id):
        # 渲染在工作线程跑，主线程仍要回 get；偶发慢给一次重试（不掩盖真故障：
        # 两次都超时才算失败）。
        last = None
        for _ in range(2):
            try:
                return self.ok("get", {"id": element_id})["props"]
            except (TimeoutError, OSError) as exc:
                last = exc
                time.sleep(0.5)
        raise last


def start(binary, shots):
    ctl = os.path.join(shots, "ctl.json")
    try:
        os.remove(ctl)
    except FileNotFoundError:
        pass
    log = open(os.path.join(shots, "app.log"), "wb")
    argv = [binary, "--headless", "--control-port", "0", "--control-file", ctl, "--shots", shots]
    process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(250):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl, encoding="utf-8"))
                if info.get("port"):
                    return process, Client(info["port"], info.get("token", "")), info
            except Exception:
                pass
        time.sleep(0.2)
    raise RuntimeError("应用未在 50s 内写出控制文件")


FAILS = []


def check(cond, msg):
    print(("  OK   " if cond else "  FAIL ") + msg)
    if not cond:
        FAILS.append(msg)


SCRIPT = {
    "name": "quality-probe",
    "fps": 12,
    "size": [320, 180],
    "transition": 0.0,
    "fade_in": 0.0,
    "fade_out": 0.0,
    "audio": {"enabled": False, "style": "calm", "bpm": 90, "gain": 0.0},
    "scenes": [
        # 1. 纯色不动 → 静止
        {"kind": "custom", "duration": 1.5, "title": "still", "background": ["#101820", "#101820"],
         "layers": [{"kind": "rect", "x": "10%", "y": "10%", "w": "30%", "h": "30%", "fill": "#e0e0e0"}]},
        # 2. 大幅运动（矩形从左侧推到右侧）→ 在动
        {"kind": "custom", "duration": 1.5, "title": "motion", "background": ["#101820", "#101820"],
         "layers": [{"kind": "rect", "x": "2%", "y": "40%", "w": "40%", "h": "20%", "fill": "#ffffff",
                     "enter": 3, "enter_at": 0.05, "enter_dur": 0.9}]},
        # 3. 全白 → 过亮
        {"kind": "custom", "duration": 1.0, "title": "white", "background": ["#ffffff", "#ffffff"],
         "layers": []},
        # 4. 全黑
        {"kind": "custom", "duration": 1.0, "title": "black", "background": ["#000000", "#000000"],
         "layers": []},
    ],
}


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    shots = os.path.join(ROOT, "build", "e2e-caiyun-quality")
    os.makedirs(shots, exist_ok=True)
    out_dir = os.path.join(shots, "render")
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)

    process, client, info = start(binary, shots)
    try:
        client.ok("hello")
        print("已连接：%s %s 后端=%s" % (info.get("app"), info.get("version"), info.get("backend")))

        print("\n[1] 应用探针脚本")
        # check() 收到 False 会判 FAIL（早先写成 check(invoke(...), ...) 时，
        # 返回 False 反而静默通过——"被拒"被打印成 OK，最坏的一种测试假绿）。
        applied = client.invoke("director", "apply_script", json.dumps(SCRIPT))
        if not applied:
            # 被拒时必须把 last_error 带出来（否则只看到一句"被拒"）
            detail = client.props("director").get("last_error") or "(无 last_error)"
            check(False, "探针脚本被拒：%s" % detail)
        else:
            check(True, "探针脚本已应用")
        time.sleep(0.2)

        print("\n[2] 渲染（小分辨率、12fps，走完整帧循环）")
        out = os.path.join(out_dir, "probe.mp4")
        os.makedirs(out_dir, exist_ok=True)
        accepted = client.invoke("director", "render", out)
        if not accepted:
            detail = client.props("director").get("last_error") or "(无 last_error)"
            check(False, "render 未被受理：%s" % detail)
        else:
            check(True, "render 已受理")

        # **不要在渲染期间高频轮询 get**：渲染线程每推进一条日志都会调
        # on_update（post 到 UI 线程），而 UI 线程若正忙于回复我们的 get，
        # 就会与渲染线程互相等——表现是控制通道超时、渲染看起来"卡住"。
        # 正确做法：给渲染留出时间，再低频查询（这也是真实使用时的姿态）。
        stage = ""
        deadline = time.time() + 240
        while time.time() < deadline:
            time.sleep(2.0)
            try:
                stage = client.props("director")["render_stage"]
            except (TimeoutError, OSError):
                continue          # 单次查询超时不算失败，继续等
            if stage in ("done", "failed", "cancelled"):
                break
        check(stage == "done", "渲染结束阶段应为 done，实为 %s" % stage)

        print("\n[3] 读画面质量属性")
        p = client.props("director")
        q = {k: p.get("render_" + k) for k in
             ("quality", "quality_still_ratio", "quality_bright_ratio",
              "quality_mean_luma", "quality_change_density", "quality_partial",
              "quality_static_threshold")}
        print("      summary:", q["quality"])
        for k, v in q.items():
            if k != "quality":
                print("      %-28s %s" % (k, v))

        still = float(q["quality_still_ratio"])
        bright = float(q["quality_bright_ratio"])
        luma = float(q["quality_mean_luma"])
        thresh = float(q["quality_static_threshold"])

        print("\n[4] 真值断言")
        check(0.0 < thresh < 5.0, "静止阈值应在 0–5 之间（实为 %s）" % thresh)
        check(q["quality_partial"] == "false", "整片渲染不该标 partial")
        # 2.5s 静止 / 4.5s 帧对 ≈ 55%；给宽区间（运动镜的入场缓动也会产生慢帧）
        check(25.0 <= still <= 75.0, "全片静止占比应落在 25–75%%，实为 %.1f" % still)
        # 1.0s 全白 / 5.0s = 20%
        check(bright >= 15.0, "过亮帧占比应 >=15%%（含 1s 全白），实为 %.1f" % bright)
        check(20.0 <= luma <= 200.0, "平均亮度应居中（黑+白+暗底混合），实为 %.1f" % luma)

        print("\n[5] 逐镜明细（quality_json）")
        raw = p.get("render_quality_json") or ""
        check(bool(raw), "quality_json 不应为空")
        if raw:
            data = json.loads(raw)
            shots_list = data.get("shots") or []
            print("      镜数:", len(shots_list))
            by_label = {}
            for s in shots_list:
                print("        %-8s still=%6.1f%%  luma=%6.1f  frames=%d"
                      % (s.get("label"), s.get("still_ratio", 0), s.get("mean_luma", 0),
                         s.get("frames", 0)))
                by_label[s.get("label")] = s
            check(len(shots_list) == 4, "应报 4 镜，实为 %d" % len(shots_list))
            if "still" in by_label:
                check(by_label["still"]["still_ratio"] > 85.0,
                      "「still」镜应 >85%% 静止，实为 %.1f" % by_label["still"]["still_ratio"])
            if "motion" in by_label:
                check(by_label["motion"]["still_ratio"] < 60.0,
                      "「motion」镜应明显更低（<60%%），实为 %.1f" % by_label["motion"]["still_ratio"])
            if "white" in by_label:
                check(by_label["white"]["mean_luma"] > 240.0,
                      "「white」镜亮度应 >240，实为 %.1f" % by_label["white"]["mean_luma"])
            if "black" in by_label:
                check(by_label["black"]["mean_luma"] < 15.0,
                      "「black」镜亮度应 <15，实为 %.1f" % by_label["black"]["mean_luma"])
            # 最静一镜应指向 still（而不是全片平均）
            worst = data.get("worst_still_clip_id")
            check(worst is not None and int(worst) > 0, "应给出最静一镜的 clip_id，实为 %s" % worst)

        print("\n" + "=" * 60)
        if FAILS:
            print("FAIL: %d 项" % len(FAILS))
            for f in FAILS:
                print("  -", f)
            return 1
        print("PASS: 画面质量度量与真值一致")
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
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    sys.exit(main())
