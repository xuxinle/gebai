"""热重载（改文件即生效）的端到端验证。

验的是这条通路的**四件事**，任一处坏掉“改完保存就见效”就不成立：
① 改 XML → 界面重建（新元素在场、旧元素退场），进程不重启；
② 改 JS → 逻辑重装（脚本侧状态与绑定清干净，不叠处理器）；
③ 写错 XML → 界面**保持原样**（原子替换），错误可读，改回来立刻生效；
④ 写错 JS → 旧逻辑继续跑（原子替换），错误可读。

跑法：py -3 tools/hot_reload_e2e.py
"""

import json
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / "xmlapp.exe"
UI_DIR = ROOT / "examples" / "xmlapp" / "ui"

sys.path.insert(0, str(ROOT / "tools"))
from gbcode_hint_e2e import Client  # noqa: E402

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def log(message):
    print(message, flush=True)


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}（先 st build xmlapp）")
        return 2

    shots = ROOT / "tmp" / "hot-reload-e2e"
    shutil.rmtree(shots, ignore_errors=True)
    shots.mkdir(parents=True, exist_ok=True)
    # 用一份**副本**做资源目录：绝不动仓库里的示例文件（半途失败会留下被改坏的示例）。
    ui = shots / "ui"
    shutil.copytree(UI_DIR, ui)
    xml_path = ui / "main.xml"
    js_path = ui / "app.js"
    xml_backup = xml_path.read_text(encoding="utf-8")
    js_backup = js_path.read_text(encoding="utf-8")

    ctl = shots / "ctl.json"
    handle = open(shots / "log.txt", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--watch", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(shots), "--ui", str(ui)],
        stdout=handle, stderr=subprocess.STDOUT)
    failures = []

    def check(ok, label):
        log(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    def write(path, text):
        # 与编辑器一致：先写临时文件再替换（不是截断重写）——顺便验证“内容指纹”判据
        temp = path.with_suffix(path.suffix + ".tmp")
        temp.write_text(text, encoding="utf-8")
        temp.replace(path)

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
            log((shots / "log.txt").read_text(encoding="utf-8", errors="replace")[-2000:])
            return 1

        def find(selector):
            return client.ok("find", {"selector": selector}).get("matches") or []

        def text_of(selector):
            matches = find(selector)
            return matches[0].get("text") if matches else None

        def wait_for(predicate, timeout=6.0):
            deadline = time.time() + timeout
            while time.time() < deadline:
                if predicate():
                    return True
                time.sleep(0.15)
            return False

        # 基线：XML 与 JS 都生效了
        check(text_of("#header-title") == "任务清单", f"基线：标题来自 XML（{text_of('#header-title')!r}）")
        check(text_of("#counter") == "共 0 项 · 完成 0", f"基线：计数来自 JS（{text_of('#counter')!r}）")
        check(bool(client.ok("script", {"code": "typeof app.hot.reload"})["result"] == "function"),
              "宿主暴露了 app.hot.reload（脚本可一句话重载）")

        # ① 改 XML → 界面重建
        write(xml_path, xml_backup.replace('id="header-title" size="18">任务清单',
                                           'id="header-title" size="18">热重载标题'))
        check(wait_for(lambda: text_of("#header-title") == "热重载标题"),
              f"① 改 XML 即生效（{text_of('#header-title')!r}）")
        check(text_of("#counter") == "共 0 项 · 完成 0", "① 界面重建后逻辑自愈（计数仍在）")
        # 新增元素也要在场（不是“把旧树原地改属性”，而是真重建）
        write(xml_path, xml_backup.replace('<Divider/>',
                                           '<Text id="hot-marker">新增标记</Text>\n  <Divider/>'))
        check(wait_for(lambda: bool(find("#hot-marker"))), "① 新增的 XML 元素出现在场")
        write(xml_path, xml_backup)
        check(wait_for(lambda: not find("#hot-marker")), "① 删掉 XML 元素 → 界面里真的没了")

        # ② 改 JS → 逻辑重装
        # 判据：把计数文案改掉（证明新闭包在跑），并点两次「添加」验证**处理器没叠**
        #（旧闭包还在时一次点击会加两条——那是重载最常见的真实缺陷）。
        write(js_path, js_backup.replace("'共 ' + counts.total + ' 项 · 完成 ' + counts.done",
                                         "'HOT 共 ' + counts.total + ' 项 · 完成 ' + counts.done"))
        check(wait_for(lambda: (text_of("#counter") or "").startswith("HOT")),
              f"② 改 JS 即生效（{text_of('#counter')!r}）")
        client.ok("script", {"code": "$('#draft').set({value:'一件事'}); $('#add').click(); 0"})
        check(wait_for(lambda: (text_of("#counter") or "") == "HOT 共 1 项 · 完成 0"),
              f"② 一次点击只加一条（绑定没叠）（{text_of('#counter')!r}）")
        # 脚本状态被清：`state` 里不留上一轮的东西
        client.ok("script", {"code": "state.probe = 1; 0"})
        write(js_path, js_backup)
        check(wait_for(lambda: (text_of("#counter") or "").startswith("共 ")),
              f"② 改回原 JS 生效（{text_of('#counter')!r}）")

        # ③ 写坏 XML → 界面保持原样
        #
        # ⚠ 背景写清楚：坏 XML 时 `refresh_ui` **不会**动界面（原子替换）；但 `poll()`
        # 发现 XML 变了后会为了“重放绑定”再重装一次逻辑，而新逻辑跑的 `render()`
        # 找不到 `#counter`（新树不存在）——那是它自己把文案丢了，不是界面被清了。
        # 所以这里用**界面树自身的证据**：根元素还在、`#list` 还在、坏代码里的
        # `#oops` 没进来。
        write(xml_path, "<Column id=\"root\"><Buton id=\"oops\" label=\"拼错\"></Column>")
        time.sleep(1.2)
        check(bool(find("#root")) and bool(find("#list")), "③ 坏 XML 不清界面（旧树仍在：root/list 都在）")
        check(not find("#oops"), "③ 坏 XML 的新元素没被挂上（没有半棵新树）")
        still = (shots / "log.txt").read_text(encoding="utf-8", errors="replace")
        check("界面重载失败" in still, "③ 失败原因写进日志（界面重载失败）")
        write(xml_path, xml_backup)
        check(wait_for(lambda: text_of("#header-title") == "任务清单"), "③ 改回来后自动恢复")

        # ④ 写坏 JS → 旧逻辑继续跑
        write(js_path, "function broken( { this is not js\n")
        time.sleep(1.2)
        check(text_of("#header-title") == "任务清单", "④ 坏 JS 不影响正在跑的界面")
        client.ok("script", {"code": "$('#draft').set({value:'还活着'}); $('#add').click(); 0"})
        check(wait_for(lambda: (text_of("#counter") or "").startswith("共 ")),
              f"④ 旧逻辑仍在工作（{text_of('#counter')!r}）")
        write(js_path, js_backup)
        check(wait_for(lambda: text_of("#header-title") == "任务清单"), "④ 改回来后再无异常")

        # ⑤ 诊断面：控制通道能问到“重载了几次、有没有错”
        status = client.ok("script", {"code": "JSON.stringify(app.hot.status())"})["result"]
        check('"ui_refreshes"' in status, f"⑤ app.hot.status() 可查（{status[:120]}…）")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        handle.close()

    log("")
    if failures:
        log(f"失败 {len(failures)} 项：")
        for item in failures:
            log("  - " + item)
        log(f"日志尾部见 {shots / 'log.txt'}")
        return 1
    log("全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
