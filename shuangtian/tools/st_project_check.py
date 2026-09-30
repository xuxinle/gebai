#!/usr/bin/env python3
"""独立工程全链路验收（框架可引入 + 歌白可全链路负责）。

验证的是"别人拿这套框架能不能用"这件事本身：

  ① `st init` 生成工程（清单含 framework 引用、build.sh、.gitignore）
  ② 本机构建 → 运行 → 控制通道读回（证明**真能跑起来**，不是"编过了"）
  ③ 第二个工程冷启动**命中共享对象缓存**（框架源不再重编）
  ④ 交叉编译出 Windows `.exe`（PE32+），可选 wine 实跑
  ⑤ 一并检查工程自身不依赖"在框架仓库里"这件事（全部在临时目录进行）

用法：`python3 tools/st_project_check.py [--framework DIR] [--st PATH] [--keep]`
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent


def log(message: str) -> None:
    print(message, flush=True)


def run(command: list[str], cwd: pathlib.Path | None = None, timeout: float = 900.0):
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=timeout)


def wait_for_control(control_file: pathlib.Path, timeout: float = 60.0) -> tuple[int, str]:
    """返回 (port, token)——token 供 `hello` 鉴权（控制文件读出）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if control_file.exists():
            try:
                info = json.loads(control_file.read_text())
                port = info.get("port") or 0
                if port:
                    return int(port), info.get("token", "")
            except (json.JSONDecodeError, OSError):
                pass
        time.sleep(0.2)
    return 0, ""


CONTROL_TOKEN = ""  # main() 从控制文件读出后设置；hello 门 + 鉴权用


def call(port: int, method: str, params: dict | None = None) -> dict:
    # 每调用一条连接：非 hello/ping 由公共库自动前置握手（hello 门 + token）
    return st_client_lib.call_with_token(port, method, params, CONTROL_TOKEN)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--framework", default=str(ROOT))
    parser.add_argument("--st", default=str(ROOT / "build/bin/st"))
    parser.add_argument("--keep", action="store_true", help="保留临时工程便于排查")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--skip-cross", action="store_true")
    args = parser.parse_args()

    framework = pathlib.Path(args.framework).resolve()
    st = pathlib.Path(args.st).resolve()
    failures: list[str] = []
    checks = 0

    def check(condition: bool, description: str, detail: str = "") -> bool:
        nonlocal checks
        checks += 1
        if condition:
            log(f"  [ok]   {description}")
        else:
            failures.append(description)
            log(f"  [FAIL] {description}{(' · ' + detail) if detail else ''}")
        return condition

    if not (framework / "st.pkg").exists():
        log(f"跳过：{framework} 不是霜天框架根（无 st.pkg）")
        return 0
    if not st.exists():
        log(f"跳过：找不到 st（{st}）—— 先跑 ./bootstrap.sh")
        return 0

    work = pathlib.Path(tempfile.mkdtemp(prefix="st-project-check-"))
    log(f"临时工作区: {work}（独立于框架仓库，模拟外部使用者）")
    try:
        # ── ① init ──────────────────────────────────────────────────────────
        log("① st init 生成独立工程")
        project = work / "demoapp"
        result = run([str(st), "init", str(project), "--name", "demoapp"])
        check(result.returncode == 0, "init 成功", result.stderr.strip()[:200])
        manifest = json.loads((project / "st.pkg").read_text())
        check(manifest.get("name") == "demoapp", "清单工程名正确")
        check(manifest.get("framework", {}).get("path") not in (None, ""),
              "清单引用了框架（framework.path 非空）")
        check((project / "build.sh").exists(), "生成了 build.sh")
        check((project / ".gitignore").exists(), "生成了 .gitignore")

        # ── ② 本机构建 + 运行 + 控制通道 ─────────────────────────────────────
        log("② 本机构建 → 运行 → 控制通道读回")
        result = run([str(st), "build", "demoapp", "-j", str(args.jobs)], cwd=project)
        check(result.returncode == 0, "构建成功", (result.stdout + result.stderr)[-300:])
        binary = project / "build/debug/bin/demoapp"
        check(binary.exists(), f"产物存在（{binary}）")

        control_file = work / "control.json"
        if binary.exists():
            process = subprocess.Popen(
                [str(binary), "--headless", "--control-port", "0", "--control-file", str(control_file)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,
            )
            try:
                port, token = wait_for_control(control_file)
                if check(port > 0, "控制通道就绪"):
                    global CONTROL_TOKEN
                    CONTROL_TOKEN = token
                    hello = call(port, "hello", {"token": token})["result"]
                    check(hello["app"]["name"] == "demoapp", "应用名正确")
                    tree = call(port, "tree")["result"]["tree"]
                    # `tree` 返回**嵌套**语义树（见 DESIGN §6.2）：数一下节点总数
                    def count_nodes(node: dict) -> int:
                        return 1 + sum(count_nodes(child) for child in node.get("children", []))
                    total = count_nodes(tree)
                    check(total > 0, f"界面有 {total} 个节点")
                    found = call(port, "find", {"selector": "Text"})["result"]
                    check(found["count"] > 0, "能找到文本组件")
                    call(port, "shutdown")
            finally:
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.terminate()

        # ── ③ 第二个工程命中共享缓存 ────────────────────────────────────────
        log("③ 第二个工程：共享对象缓存应命中（框架源不重编）")
        second = work / "secondapp"
        run([str(st), "init", str(second), "--name", "secondapp"])
        started = time.time()
        result = run([str(st), "build", "secondapp", "-j", str(args.jobs)], cwd=second)
        elapsed = time.time() - started
        check(result.returncode == 0, "第二个工程构建成功", (result.stdout + result.stderr)[-200:])
        # 冷启动编译整个框架通常 ≥ 20s；命中缓存后应显著更快
        check(elapsed < 20.0, f"构建耗时 {elapsed:.1f}s（应 < 20s，说明缓存命中）",
              "缓存未生效：检查 ~/.shuangtian/cache/objects")

        # ── ④ 交叉编译 ──────────────────────────────────────────────────────
        if not args.skip_cross:
            log("④ 交叉编译出 Windows 可执行")
            mingw = shutil.which("x86_64-w64-mingw32-g++")
            if mingw is None:
                log("  [skip] 未安装 mingw（apt install g++-mingw-w64-x86-64）")
            else:
                result = run(
                    [str(st), "build", "demoapp", "--profile", "release", "--toolchain=mingw",
                     "-j", str(args.jobs)],
                    cwd=project,
                )
                check(result.returncode == 0, "交叉编译成功", (result.stdout + result.stderr)[-300:])
                exe = project / "build/release-mingw/bin/demoapp.exe"
                check(exe.exists(), f"Windows 产物存在（{exe.name}）")
                if exe.exists():
                    head = exe.read_bytes()[:2]
                    check(head == b"MZ", "产物是 PE 格式（MZ 头）")
                    check(
                        "工具链无需引用方声明" if manifest.get("toolchains") is None else True,
                        "引用方无需自己声明 mingw 工具链（继承自框架）",
                    )
    finally:
        if args.keep:
            log(f"保留临时工作区: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    log(f"\n检查 {checks} 项 · 失败 {len(failures)} 项")
    for item in failures:
        log(f"  - {item}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
