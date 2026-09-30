#!/usr/bin/env python3
"""霜天控制通道最小客户端（联调/验证用）：发送若干请求并打印结果。

用法: python3 st_ctl.py <port|控制文件> <method> [json-params] [method] [params] ...
      python3 st_ctl.py 9701 hello '{}' tree '{"depth":3}' capture '{"encode":"file","path":"/tmp/a.png"}'
      python3 st_ctl.py /tmp/st-ctl.json hello '{}' tree '{"depth":3}'   # 推荐：从控制文件读 port+token
      ST_TOKEN 环境变量可显式提供 hello 的鉴权 token（传 port 而非控制文件时用）
"""
import json
import os
import socket
import struct
import sys


def call(sock: socket.socket, request_id: int, method: str, params: dict) -> dict:
    body = json.dumps({"id": request_id, "method": method, "params": params}).encode()
    sock.sendall(struct.pack(">I", len(body)) + body)
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            raise RuntimeError("连接被关闭")
        header += chunk
    (length,) = struct.unpack(">I", header)
    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise RuntimeError("响应截断")
        payload += chunk
    return json.loads(payload)


def main() -> int:
    target = sys.argv[1]
    token = os.environ.get("ST_TOKEN", "")
    if os.path.isfile(target):
        # 传入控制文件路径：port 与 token 一并读出（token 是 hello 鉴权必需，推荐用法）
        with open(target, "r", encoding="utf-8") as handle:
            info = json.load(handle)
        port = int(info.get("port") or 0)
        token = info.get("token", token)
    else:
        port = int(target)
    pairs = sys.argv[2:]
    sock = socket.create_connection(("127.0.0.1", port), timeout=60)
    request_id = 0
    index = 0
    while index < len(pairs):
        method = pairs[index]
        params = json.loads(pairs[index + 1]) if index + 1 < len(pairs) else {}
        index += 2
        request_id += 1
        if method == "hello" and token:
            params.setdefault("token", token)
        reply = call(sock, request_id, method, params)
        ok = reply.get("ok")
        print(f"=== {method} -> ok={ok} ===")
        text = json.dumps(reply.get("result") if ok else reply.get("error"),
                          ensure_ascii=False, indent=2)
        print(text[:4000])
    sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
