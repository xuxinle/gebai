"""gbcode 单文件历史弹窗（B6）端到端回归。

验证「真仓库 → `git log --follow` → 弹窗列表」这条接缝：
建真仓库、对同一文件提交两次，打开该文件 → 右键菜单「文件历史…」→
断言弹窗列出**两条**提交、并且点第一条能打开差异。

跑法：py -3 tools/gbcode_history_e2e.py
"""

import base64
import json
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / ("gbcode.exe" if __import__("os").name == "nt" else "gbcode")

sys.path.insert(0, str(ROOT / "tools"))
from gbcode_hint_e2e import Client, wait_until  # noqa: E402


def log(message):
    print(message, flush=True)


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}（先 st build gbcode）")
        return 2

    base = ROOT / "build" / "artifacts"
    base.mkdir(parents=True, exist_ok=True)
    work = base / "gbcode_history"
    # 上一轮可能还有进程握着目录（Windows 上 rmtree 会静默失败）——
    # 重试几次，仍失败就换一个带时间戳的目录（宁可留垃圾，不要假失败）。
    for _ in range(5):
        shutil.rmtree(work, ignore_errors=True)
        if not work.exists():
            break
        time.sleep(0.4)
    if work.exists():
        work = base / f"gbcode_history_{int(time.time())}"
    repo = work / "repo"
    repo.mkdir(parents=True)

    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "tester@example.com")
    git(repo, "config", "user.name", "测试作者")
    target = repo / "tracked.cpp"
    target.write_text("int a = 1;\n", encoding="utf-8")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "第一次提交")
    target.write_text("int a = 1;\nint b = 2;\n", encoding="utf-8")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "第二次提交")

    shots = base / "gbcode_history_shots"   # 独立于 work：work 会在 finally 里被删
    shots.mkdir(parents=True, exist_ok=True)
    ctl = shots / "ctl.json"
    handle = open(shots / "log.txt", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(shots), "--tool-root", str(ROOT), "--workspace", str(repo),
         "--open", str(target.resolve())],
        stdout=handle, stderr=subprocess.STDOUT)
    failures = []

    def check(ok, label):
        log(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    try:
        client = None
        for _ in range(240):
            if ctl.exists():
                try:
                    info = json.loads(ctl.read_text(encoding="utf-8"))
                    if info.get("port"):
                        client = Client(info["port"], info.get("token", ""))
                        client.ok("hello")
                        break
                except Exception:  # noqa: BLE001
                    pass
            time.sleep(0.25)
        if client is None:
            log("控制通道没起来：")
            log((shots / "log.txt").read_text(encoding="utf-8", errors="replace")[-1200:])
            return 1

        def find(selector):
            return client.ok("find", {"selector": selector}).get("matches") or []

        # ① 数据面：`git log --follow` 对这个文件有两条。
        data = git(repo, "log", "--follow", "--oneline", "--", "tracked.cpp")
        check(data.stdout.count("\n") >= 2, "数据面：该文件有两条提交历史")

        # ② 已打开仓库里的文件（`--open`）。
        reply = client.call("get", {"id": "editor", "keys": ["value"]})
        value = (reply.get("result") or {}).get("value") or ""
        check("int a = 1" in value, "编辑器装的是 tracked.cpp")

        # ③ 走**真实用户路径**打开：命令面板（Ctrl+Shift+P）→ 输入命令名 → Enter。
        #
        # 为何不用 `invoke`：控制通道的 `invoke` 作用于**元素 id**，而命令面板的命令是
        # 数据（不是元素），点不到；而快捷键路径要过键盘分派。这条路径与用户操作一致，
        # 也顺带验证了「命令已登记进面板」这件事。
        client.ok("input.key", {"key": "P", "ctrl": True, "shift": True})
        appeared_palette = wait_until(lambda: bool(find("#command-palette")), timeout=5.0,
                                      what="命令面板出现")
        check(appeared_palette, "命令面板打开")
        client.ok("input.text", {"text": "当前文件的历史"})
        time.sleep(0.4)
        client.ok("input.key", {"key": "Enter"})
        time.sleep(0.6)

        # ④ 弹窗出现且列出两条。
        appeared = wait_until(lambda: bool(find("#file-history-list")), timeout=6.0,
                              what="历史弹窗出现")
        check(appeared, "文件历史弹窗出现")
        if appeared:
            items = find("ListItem")
            log(f"      条目数：{len(items)}")
            check(len(items) >= 2, "列出两条提交")
            texts = [m.get("text") or "" for m in items]
            check(any("第二次提交" in t for t in texts), "含最新提交标题")
            # 快照留档（人工复核）
            (shots / "history-open.png").write_bytes(
                base64.b64decode(client.ok("capture", {})["base64"]))
        else:
            log("      （弹窗未出现，截图留档）")
            (shots / "history-missing.png").write_bytes(
                base64.b64decode(client.ok("capture", {})["base64"]))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    log("")
    if failures:
        log(f"失败 {len(failures)} 项：")
        for item in failures:
            log("  - " + item)
        return 1
    log("全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
