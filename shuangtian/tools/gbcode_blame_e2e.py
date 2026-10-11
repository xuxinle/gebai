"""gbcode 行尾 blame 注解（B5）端到端回归。

验证的是「真仓库 → 数据面 → 界面」这条完整接缝：建一个真 git 仓库、真提交，
开 blame 开关，断言**注解真的进了编辑器的行装饰**（不是只看 setter 有没有被调）。

跑法：py -3 tools/gbcode_blame_e2e.py
"""

import base64
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / "gbcode.exe"

sys.path.insert(0, str(ROOT / "tools"))
from gbcode_hint_e2e import Client, wait_until  # noqa: E402


def log(message):
    print(message, flush=True)


def git(cwd, *args):
    # `encoding="utf-8"` + `errors="replace"`：git 输出是 UTF-8（含中文作者名），
    # 而 Windows 的 `subprocess` 默认用 gbk 解码——实测抛 `UnicodeDecodeError`，
    # 于是 `stdout` 是 `None`，后续断言直接 TypeError（而非有用的失败信息）。
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}（先 st build gbcode）")
        return 2

    # ⚠ 别用 `%TEMP%`：Windows 会把它短名化成 `ADMINI~1`，而短名路径在
    # `git blame <path>` 里算不出仓库相对路径——表现为"注解一行都不出现"
    #（实测踩到：open_path 收到的是 `C:\Users\ADMINI~1\...`）。
    base = ROOT / "build" / "artifacts"
    base.mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_blame_", dir=base))
    repo = work / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "tester@example.com")
    git(repo, "config", "user.name", "测试作者")
    (repo / "sample.cpp").write_text("int main() {\n  return 0;\n}\n", encoding="utf-8")
    git(repo, "add", "-A")
    if git(repo, "commit", "-q", "-m", "初始提交").returncode != 0:
        log("建仓提交失败")
        return 2

    shots = ROOT / "build" / "artifacts" / "gbcode-blame-shots"   # 独立于 work：work 会被删
    shutil.rmtree(shots, ignore_errors=True)
    shots.mkdir(parents=True, exist_ok=True)
    ctl = shots / "ctl.json"
    handle = open(shots / "log.txt", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(shots), "--tool-root", str(ROOT), "--workspace", str(repo),
         "--open", str((repo / "sample.cpp").resolve())],
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

        # ① 数据面（独立于应用）：`git blame` 真能读到作者。
        blame = git(repo, "blame", "--line-porcelain", "sample.cpp")
        check(blame.returncode == 0 and "测试作者" in blame.stdout,
              "数据面：git blame 读到作者")

        # ② 确认打开的是**仓库里的文件**（`--open` 在启动时已打开）。
        #
        # ⚠ 这一步是必要的：默认打开的是内置样例 `renderer.cpp`，而 blame 要的是
        # 仓库里的文件——不在仓库里的文件取不到行信息，注解自然是空的。
        # 实测踩到：开关、取数、绘制全对，但界面上一行注解都不出现（看的是另一个文件）。
        # ⚠ 判据用**编辑器自己的内容**（`get #editor value`）而不是标签类型名——
        # 标签的类型名随实现变（实测 `Tab` 匹配不到），而“编辑器里是哪个文件的内容”
        # 才是本节真正要确认的事（它曾经错了：标签切了、内容还是上一个文件）。
        editor_value = ""
        reply = client.call("get", {"id": "editor", "keys": ["value"]})
        editor_value = (reply.get("result") or {}).get("value") or ""
        log(f"      编辑器首行：{json.dumps(editor_value.splitlines()[:1], ensure_ascii=False)}")
        check("int main" in editor_value, "编辑器装的是仓库里的 sample.cpp（不是默认样例）")

        # ③ 开关在场，且初始是「关」。
        switch = find("#status-blame")
        check(bool(switch), "状态栏 blame 开关在场")
        label_off = (switch[0].get("text") if switch else "") or ""
        check(label_off == "blame", f"初始为关（标签 {label_off!r}）")

        # ③ 点开：标签变「blame 开」。
        client.ok("invoke", {"id": "status-blame", "action": "click"})
        on = wait_until(lambda: (find("#status-blame") or [{}])[0].get("text") == "blame 开",
                        timeout=5.0, what="开关变开")
        check(on, "点击后标签变「blame 开」")

        # ④ **界面面**：编辑器里真的出现了行尾注解。
        #
        # 判据用**画面**而不是内部字段：注解是像素级的产物，而 `end_text` 有没有进
        # `line_decorations` 是内部状态（单测已覆盖）。这里要回答的是
        # “用户看得见吗”，所以比开启前后**同一片区域**的截图哈希。
        editor = find("CodeEditor")
        check(bool(editor), "编辑器在场")
        if editor:
            rect = editor[0]["bounds"]
            # 取编辑器正文区（避开状态栏与侧栏）。
            def region_digest():
                raw = base64.b64decode(client.ok("capture", {})["base64"])
                path = shots / "shot.png"
                path.write_bytes(raw)
                # 用像素统计做指纹：先把 PNG 解码（无 Pillow 时退化为文件哈希——
                # 那样会被时间戳类噪声干扰，故优先用 Pillow）。
                try:
                    from PIL import Image  # noqa: PLC0415
                except ImportError:
                    return None
                with Image.open(path) as image:
                    box = (int(rect["x"]), int(rect["y"]),
                           int(rect["x"] + rect["width"]), int(rect["y"] + rect["height"]))
                    return hash(image.crop(box).tobytes())

            before = region_digest()
            if before is None:
                log("  SKIP 无 Pillow，跳过画面指纹（数据面与开关已验）")
            else:
                # 再切一次关→开，看画面是否变化（关闭态是基准）。
                client.ok("invoke", {"id": "status-blame", "action": "click"})
                wait_until(lambda: (find("#status-blame") or [{}])[0].get("text") == "blame",
                           timeout=5.0, what="开关变关")
                off_digest = region_digest()
                client.ok("invoke", {"id": "status-blame", "action": "click"})
                wait_until(lambda: (find("#status-blame") or [{}])[0].get("text") == "blame 开",
                           timeout=5.0, what="开关变开")
                time.sleep(0.5)   # 给一帧取数 + 重绘
                on_digest = region_digest()
                check(off_digest != on_digest, "开启后编辑器区域画面变化（注解画出来了）")

        (shots / "final.png").write_bytes(
            base64.b64decode(client.ok("capture", {})["base64"]))
        check((shots / "final.png").exists(), "截图落盘（人工复核）")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(work, ignore_errors=True)

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
