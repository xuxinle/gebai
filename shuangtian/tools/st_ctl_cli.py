#!/usr/bin/env python3
"""最小控制通道 CLI：一条命令 = 一个调用（调试用，不做断言）。

用法:
  python3 tools/st_ctl_cli.py <ctl.json> <method> [json_params]

例:
  python3 tools/st_ctl_cli.py /tmp/st-ctl.json tree
  python3 tools/st_ctl_cli.py /tmp/st-ctl.json invoke '{"id":"decl-inc","action":"click"}'
"""
import json
import socket
import struct
import sys


def call(sock, request_id, method, params, token):
    if method == "hello":
        params = dict(params, token=token)
    body = json.dumps({"id": request_id, "method": method, "params": params}).encode()
    sock.sendall(struct.pack(">I", len(body)) + body)
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            raise RuntimeError("连接在收到响应头前关闭")
        header += chunk
    length = struct.unpack(">I", header)[0]
    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise RuntimeError("连接在收到完整响应前关闭")
        payload += chunk
    return json.loads(payload)


def main():
    ctl_path, method = sys.argv[1], sys.argv[2]
    params = json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}
    info = json.load(open(ctl_path, encoding="utf-8"))
    sock = socket.create_connection(("127.0.0.1", info["port"]), timeout=30.0)
    sock.settimeout(30.0)
    hello = call(sock, 0, "hello", {}, info.get("token", ""))
    if not hello.get("ok"):
        print(json.dumps(hello, ensure_ascii=False, indent=2))
        return 1
    reply = call(sock, 1, method, params, info.get("token", ""))
    print(json.dumps(reply, ensure_ascii=False, indent=2))
    return 0 if reply.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
