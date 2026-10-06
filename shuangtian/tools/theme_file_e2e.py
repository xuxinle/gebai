#!/usr/bin/env python3
"""校验自定义主题文件的装载路径（`--theme-file` 与 `ST_THEME_FILE` 两条）。

为什么单写一条：主题文件的解析有单测，但「命令行/环境变量→`Application`→`UiRoot`」
这一段接线**只有起真进程才能验**——`app.cpp` 里漏个赋值不会让任何单测变红，
而用户看到的是"文件写了、没生效"（与 `ClassGammas` 那次漏接线同一个坑）。

用法：python3 tools/theme_file_e2e.py <主题文件路径>
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time

GALLERY = "build/dev/bin/gallery"
CTL_DIR = "/tmp/theme-file-e2e"


def call(port: int, token: str, method: str, params: dict) -> dict:
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    try:
        hello = json.dumps({"id": 1, "method": "hello", "params": {"token": token}}).encode()
        sock.sendall(struct.pack(">I", len(hello)) + hello)
        sock.recv(65536)
        body = json.dumps({"id": 2, "method": method, "params": params}).encode()
        sock.sendall(struct.pack(">I", len(body)) + body)
        size = struct.unpack(">I", sock.recv(4))[0]
        data = b""
        while len(data) < size:
            data += sock.recv(size - len(data))
        return json.loads(data)
    finally:
        sock.close()


def launch(theme_path: str, env: bool) -> dict:
    os.makedirs(CTL_DIR, exist_ok=True)
    ctl = os.path.join(CTL_DIR, "env.json" if env else "arg.json")
    if os.path.exists(ctl):
        os.remove(ctl)
    child_env = dict(os.environ)
    argv = [GALLERY, "--headless", "--control-file", ctl, "--frames", "6000"]
    if env:
        child_env["ST_THEME_FILE"] = theme_path
    else:
        argv += ["--theme-file", theme_path]
    proc = subprocess.Popen(argv, env=child_env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        for _ in range(80):
            time.sleep(0.15)
            if os.path.exists(ctl):
                try:
                    info = json.load(open(ctl))
                except (json.JSONDecodeError, OSError):
                    continue
                if info.get("port"):
                    return {"proc": proc, "info": info}
        raise RuntimeError("控制通道未就绪")
    except Exception:
        proc.kill()
        raise


def normalize(value):
    """把主题文件里的**书写形式**归一成回读形式，再做比较。

    为什么需要：`theme` 的快照是**规范化输出**（颜色一律 `#RRGGBB[AA]`、
    数字经 float 往返），而主题文件里写的是各种等价写法（`rgba(255,255,255,0.14)`、
    `1.6`）。直接比字面量会把"已经生效"报成"没生效"——实测就报了一屏假失败。
    """
    if isinstance(value, str):
        text = value.strip()
        if text.startswith("rgba(") or text.startswith("rgb("):
            inside = text[text.find("(") + 1:text.rfind(")")]
            parts = [p.strip() for p in inside.split(",")]
            r, g, b = (float(p) for p in parts[:3])
            a = parts[3] if len(parts) > 3 else "1"
            alpha = float(a[:-1]) / 100.0 if a.endswith("%") else float(a)
            alpha8 = round(alpha * 255)
            base = "#%02X%02X%02X" % (round(r), round(g), round(b))
            return base if alpha8 >= 255 else base + "%02X" % alpha8
        return text.upper()
    if isinstance(value, (int, float)):
        return round(float(value), 4)
    return value


def check(label: str, theme_path: str, env: bool, expectations: dict) -> bool:
    handle = launch(theme_path, env)
    proc, info = handle["proc"], handle["info"]
    ok = True
    try:
        reply = call(info["port"], info.get("token", ""), "theme", {"tokens": True})
        result = reply.get("result") or {}
        colors = ((result.get("tokens") or {}).get("colors") or {})
        metrics = ((result.get("tokens") or {}).get("metrics") or {})
        name = (result.get("tokens") or {}).get("name")
        print(f"[{label}] mode={result.get('mode')} name={name}")
        for key, want in expectations.items():
            got = colors.get(key, metrics.get(key))
            good = normalize(got) == normalize(want)
            ok = ok and good
            print(f"   {'OK ' if good else 'XX '} {key:16} = {got!r}  期望 {want!r}")
    finally:
        try:
            call(info["port"], info.get("token", ""), "shutdown", {})
        except Exception:
            pass
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
    return ok


def main() -> int:
    if len(sys.argv) < 2:
        print("用法: theme_file_e2e.py <主题文件路径>")
        return 2
    theme_path = os.path.abspath(sys.argv[1])
    # 期望值取的是主题文件本身，避免在测试里再抄一遍常量（抄了就两边可能漂移）。
    spec = json.load(open(theme_path))
    expectations = dict(spec.get("colors") or {})
    for key, value in (spec.get("metrics") or {}).items():
        expectations[key] = value
    ok = True
    ok = check("--theme-file", theme_path, env=False, expectations=expectations) and ok
    ok = check("ST_THEME_FILE", theme_path, env=True, expectations=expectations) and ok
    print("\n" + ("两条装载路径都生效。" if ok else "存在未生效的 token。"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
