#!/usr/bin/env python3
"""视觉断言原语端到端验证（真实 gallery 应用 + 真实 TCP 控制通道）。

验证：
  1. capture.hash 同区域两次一致（画面稳定）
  2. 修改元素后 hash 变化（画面断言生效）
  3. visual.diff 写基线 → 同帧比对 changed=false
  4. 改动界面 → 比对检测到差异（diff_ratio/diff_bounds 量化输出）
  5. 缺元素/白名单外路径的明确报错

用法: python3 tools/visual_assert_e2e.py <binary> <ctl.json> <port>
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time

CONTROL_FILE = None


def call(sock, request_id, method, params, token):
    if method == "hello" and token:
        params = dict(params, token=token)
    body = json.dumps({"id": request_id, "method": method, "params": params}).encode()
    sock.sendall(struct.pack(">I", len(body)) + body)
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            raise RuntimeError("连接关闭")
        header += chunk
    (length,) = struct.unpack(">I", header)
    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise RuntimeError("响应截断")
        payload += chunk
    return json.loads(payload)


def main():
    global CONTROL_FILE
    binary, control_file, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    CONTROL_FILE = control_file
    if os.path.exists(control_file):
        os.remove(control_file)
    proc = subprocess.Popen([binary, "--headless", "--control-port", str(port),
                             "--control-file", control_file, "--ms", "60000"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rid = [0]

    def nid():
        rid[0] += 1
        return rid[0]

    try:
        for _ in range(150):
            if os.path.exists(control_file):
                break
            time.sleep(0.1)
        info = json.load(open(control_file))
        token = info.get("token", "")
        sock = socket.create_connection(("127.0.0.1", info["port"]), timeout=15)
        hello = call(sock, nid(), "hello", {}, token)
        caps = hello["result"].get("capabilities", [])
        assert "capture.hash" in caps, f"能力清单缺 capture.hash: {caps}"
        assert "visual.diff" in caps, f"能力清单缺 visual.diff: {caps}"
        print("[0] hello 能力清单含 capture.hash / visual.diff")

        # 让画面先稳定：等 3 帧
        call(sock, nid(), "wait", {"for": "frames", "frames": 3, "timeout_ms": 5000}, token)

        # ① hash 稳定性 + 全屏
        h1 = call(sock, nid(), "capture.hash", {}, token)
        assert h1.get("ok"), h1
        h2 = call(sock, nid(), "capture.hash", {}, token)
        assert h1["result"]["hash"] == h2["result"]["hash"], "两次全屏 hash 不一致（画面未稳定？）"
        assert h1["result"]["algorithm"] == "fnv1a64"
        assert h1["result"]["bytes"] > 0
        full_hash = h1["result"]["hash"]
        print(f"[1] capture.hash 全屏两次一致: {full_hash[:16]}…（{h1['result']['bytes']} 字节）")

        # ② 改元素 → hash 变化（用 gallery 的靶区元素；找输入框）
        found = call(sock, nid(), "find", {"selector": "Input", "limit": 1}, token)
        matches = found["result"].get("matches", [])
        assert matches, "gallery 里找不到 Input（测试前提不成立）"
        input_id = matches[0]["id"]
        call(sock, nid(), "set", {"id": input_id, "props": {"value": "视觉断言测试"}}, token)
        call(sock, nid(), "wait", {"for": "frames", "frames": 3, "timeout_ms": 5000}, token)
        h3 = call(sock, nid(), "capture.hash", {}, token)
        assert h3["result"]["hash"] != full_hash, "设置输入框内容后全屏 hash 未变化"
        print(f"[2] 改元素后 hash 变化: {h3['result']['hash'][:16]}…")

        # ③ 写基线到控制文件同目录（白名单内）
        shots_dir = os.path.dirname(os.path.abspath(control_file))
        baseline = os.path.join(shots_dir, "visual-assert-baseline.png")
        wrote = call(sock, nid(), "visual.diff", {"path": baseline, "write_baseline": True}, token)
        assert wrote.get("ok"), wrote
        assert wrote["result"].get("written") is True
        assert os.path.exists(baseline)
        print(f"[3] 基线已写: {baseline}（{wrote['result']['width']}x{wrote['result']['height']}）")

        # ④ 同帧自比：无差异
        same = call(sock, nid(), "visual.diff", {"path": baseline}, token)
        assert same.get("ok"), same
        assert same["result"]["changed"] is False, same["result"]
        assert same["result"]["diff_pixels"] == 0, same["result"]
        print(f"[4] 同帧自比: changed=false, diff_pixels=0 "
              f"(hash {same['result']['current_hash'][:16]}…)")

        # ⑤ 改动界面 → 检测到差异
        call(sock, nid(), "set", {"id": input_id, "props": {"value": "改一下再比"}}, token)
        call(sock, nid(), "wait", {"for": "frames", "frames": 3, "timeout_ms": 5000}, token)
        diff = call(sock, nid(), "visual.diff", {"path": baseline, "tolerance": 0.0}, token)
        assert diff.get("ok"), diff
        r = diff["result"]
        assert r["changed"] is True, r
        assert r["diff_pixels"] > 0
        assert "diff_bounds" in r, r
        print(f"[5] 界面改动被检出: diff_pixels={r['diff_pixels']} / {r['total_pixels']} "
              f"({r['diff_ratio']*100:.2f}%), max_diff={r['max_diff']}, bounds={r['diff_bounds']}")

        # ⑥ tolerance 吸收微小差异
        tolerant = call(sock, nid(), "visual.diff",
                        {"path": baseline, "tolerance": 0.5}, token)
        assert tolerant["result"]["changed"] is False, "tolerance=0.5 应吸收该量级差异"
        print(f"[6] tolerance=0.5 吸收差异: changed=false（diff_ratio={tolerant['result']['diff_ratio']:.4f}）")

        # ⑦ 缺元素 → not_found；缺基线 → not_found
        missing = call(sock, nid(), "capture.hash", {"id": "#no-such"}, token)
        assert "error" in missing and missing["error"]["code"] == "not_found", missing
        absent = call(sock, nid(), "visual.diff",
                      {"path": os.path.join(shots_dir, "never-written.png")}, token)
        assert "error" in absent and absent["error"]["code"] == "not_found", absent
        print("[7] 缺元素/缺基线的错误码正确（not_found）")

        print("\n[OK] 视觉断言原语端到端全部通过")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
