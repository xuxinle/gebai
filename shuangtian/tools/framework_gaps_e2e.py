#!/usr/bin/env python3
"""五项新能力的端到端复验（控制通道驱动，跑在真实应用上）。

验证：
  1. `ui.create` 在线建元素 → 成为真值树一员（tree 可见、get/set 可用）
  2. `ui.remove` 回收
  3. `ui.create` 未知类型 → 明确报错
  4. hello 能力清单含 `ui.create` / `ui.remove`
  5. 声明式应用（codeeditor-dsl）在插入自建元素后仍正常工作

用法: python3 tools/framework_gaps_e2e.py <binary> <ctl.json> <port>
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time


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


def flatten(node, out=None):
    if out is None:
        out = []
    if isinstance(node, dict):
        if node.get("type"):
            out.append(node)
        for child in node.get("children", []) or []:
            flatten(child, out)
    return out


def main():
    binary, control_file, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    if os.path.exists(control_file):
        os.remove(control_file)
    proc = subprocess.Popen([binary, "--headless", "--control-port", str(port),
                             "--control-file", control_file, "--ms", "40000"],
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
        sock = socket.create_connection(("127.0.0.1", info["port"]), timeout=10)
        hello = call(sock, nid(), "hello", {}, token)
        caps = hello["result"].get("capabilities", [])
        assert "ui.create" in caps, f"能力清单缺 ui.create: {caps}"
        assert "ui.remove" in caps, f"能力清单缺 ui.remove: {caps}"
        print("[1] hello 能力清单含 ui.create / ui.remove")

        # 建一个容器 + 一个按钮
        r = call(sock, nid(), "ui.create",
                 {"type": "Panel", "id": "e2e-panel", "props": {"direction": "column"}}, token)
        assert r.get("ok"), f"建面板失败: {r}"
        assert r["result"]["id"] == "e2e-panel", r
        r2 = call(sock, nid(), "ui.create",
                  {"type": "Button", "parent": "e2e-panel", "key": "e2e-btn",
                   "props": {"label": "E2E"}}, token)
        assert r2.get("ok"), f"建按钮失败: {r2}"
        btn_id = r2["result"]["id"]
        print(f"[2] ui.create 建出 {btn_id}")

        # 真值树可见
        nodes = flatten(call(sock, nid(), "tree", {}, token)["result"]["tree"])
        assert any(str(n.get("id", "")).endswith("e2e-panel") for n in nodes), "面板不在树里"
        assert any(str(n.get("id", "")) == btn_id for n in nodes), "按钮不在树里"
        # get/set 照常（同一份语义）
        got = call(sock, nid(), "get", {"id": btn_id}, token)
        assert got["result"]["props"].get("label") == "E2E", got["result"]["props"]
        call(sock, nid(), "set", {"id": btn_id, "props": {"label": "E2E-2"}}, token)
        again = call(sock, nid(), "get", {"id": btn_id}, token)
        assert again["result"]["props"].get("label") == "E2E-2", again["result"]["props"]
        print("[3] 建出的元素是真值树一员（tree/get/set 全部可用）")

        # 未知类型 → 明确报错
        bad = call(sock, nid(), "ui.create", {"type": "NoSuchWidget"}, token)
        assert "error" in bad, f"未知类型未报错: {bad}"
        print(f"[4] 未知类型明确报错（{bad['error'].get('code', '?')}）")

        # 删（父删子随之）
        rm = call(sock, nid(), "ui.remove", {"id": "e2e-panel"}, token)
        assert rm.get("ok") and rm["result"].get("removed"), rm
        nodes = flatten(call(sock, nid(), "tree", {}, token)["result"]["tree"])
        assert not any(str(n.get("id", "")).endswith("e2e-panel") for n in nodes), "面板未删"
        assert not any(str(n.get("id", "")) == btn_id for n in nodes), "子元素未随之消失"
        print("[5] ui.remove 删除生效（子元素随之消失）")

        # 声明式应用仍正常（旧功能没被新能力打断）
        nodes = flatten(call(sock, nid(), "tree", {}, token)["result"]["tree"])
        ids = {str(n.get("id", "")) for n in nodes}
        assert any(i.endswith("menubar") for i in ids), "菜单栏不见了"
        assert any(i.endswith("editor") for i in ids), "编辑器不见了"
        print("[6] 声明式应用本体未受影响（菜单栏/编辑器仍在）")

        print("\n[OK] 五项框架新能力端到端全部通过")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
