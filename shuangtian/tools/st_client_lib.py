#!/usr/bin/env python3
"""控制通道客户端公共小工具：控制文件读取 + 带握手/token 的单次调用。

背景（2026-09-30 审视 P0）：控制通道加了鉴权 token（写进控制文件），且服务端要求
每连接**首帧必须是 `hello`**（或 ping）——未握手连接调用其他方法会被拒并断连。
联调脚本惯用「一次调用一条连接」，因此每次都要在同一条连接里先发 hello（与请求同批
写出，不额外等往返；服务端的 hello 响应被丢弃）。

用法：
    from st_client_lib import load_control, call_with_token
    info = load_control(ctl_path)          # {"port": ..., "token": ...}
    reply = call_with_token(info["port"], "tree", {"depth": 2}, info.get("token", ""))
"""
import json
import socket
import struct


def load_control(ctl_path):
    """读控制文件（port/pid/token/...）；不存在或损坏返回 {}。"""
    try:
        with open(ctl_path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except Exception:
        return {}


def call_with_token(port, method, params=None, token="", timeout=60, host="127.0.0.1"):
    """单次调用（每次新建连接）：非 hello/ping 自动前置握手（同批写出）。

    返回 id==1 的响应 dict（即用户请求的响应；合成 hello 的响应被丢弃）。
    """
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        frames = []
        if method not in ("hello", "ping"):
            hello_params = {}
            if token:
                hello_params["token"] = token
            frames.append((0, "hello", hello_params))
        merged = dict(params or {})
        if method == "hello" and token and "token" not in merged:
            merged["token"] = token
        frames.append((1, method, merged))
        for mid, name, payload in frames:
            body = json.dumps({"id": mid, "method": name, "params": payload}).encode()
            sock.sendall(struct.pack(">I", len(body)) + body)
        reply = None
        for _ in frames:
            header = b""
            while len(header) < 4:
                chunk = sock.recv(4 - len(header))
                if not chunk:
                    raise RuntimeError("连接被对端关闭（进程可能已崩溃/token 被拒）")
                header += chunk
            (length,) = struct.unpack(">I", header)
            if length == 0 or length > 64 * 1024 * 1024:
                raise RuntimeError(f"帧长度异常: {length}")
            payload = b""
            while len(payload) < length:
                chunk = sock.recv(length - len(payload))
                if not chunk:
                    raise RuntimeError("响应截断（进程可能已崩溃）")
                payload += chunk
            message = json.loads(payload)
            if message.get("id") == 1:
                reply = message
        if reply is None:
            raise RuntimeError("未收到目标响应（协议异常）")
        return reply
    finally:
        sock.close()
