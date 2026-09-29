#!/usr/bin/env python3
"""霜天控制通道最小客户端（联调/验证用）：发送若干请求并打印结果。

用法: python3 st_ctl.py <port> <method> [json-params] [method] [params] ...
      python3 st_ctl.py 9701 hello '{}' tree '{"depth":3}' capture '{"encode":"file","path":"/tmp/a.png"}'
"""
import json
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
    port = int(sys.argv[1])
    pairs = sys.argv[2:]
    sock = socket.create_connection(("127.0.0.1", port), timeout=60)
    request_id = 0
    index = 0
    while index < len(pairs):
        method = pairs[index]
        params = json.loads(pairs[index + 1]) if index + 1 < len(pairs) else {}
        index += 2
        request_id += 1
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
