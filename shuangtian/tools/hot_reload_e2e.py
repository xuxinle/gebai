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
BIN = ROOT / "build" / "dev" / "bin" / "xiangqi.exe"
UI_DIR = ROOT / "examples" / "xiangqi" / "ui"

sys.path.insert(0, str(ROOT / "tools"))
from gbcode_hint_e2e import Client  # noqa: E402

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def log(message):
    print(message, flush=True)


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}（先 st build xiangqi）")
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
    # `--home` 指向本次产物目录：相期会把主题/已阅记录写到 home，
    # 不重定向就会把当前用例的偏好泄到开发者的真家目录（也会反向污染下一轮）。
    # `--agent` 指向一个必然拒绝的端口：本用例只验热重载，不猜真实控制器。
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--watch", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(shots), "--ui", str(ui), "--home", str(shots / "home"),
         "--agent", "ws://127.0.0.1:1/xiangqi"],
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

        def hot_status():
            raw = client.ok("script", {"code": "JSON.stringify(app.hot.status())"})["result"]
            return json.loads(raw) if raw else {}

        def hot_bindings():
            return int(hot_status().get("bindings") or 0)

        # 基线：XML 与 JS 都生效了。
        # 相期的这两个锚点分别代表两层：`#empty-title` 的文案来自 XML，
        # `#badge` 的文案由 JS 渲染（空态时写「暂无待办」）。
        check(text_of("#empty-title") == "这里空着",
              f"基线：空态标题来自 XML（{text_of('#empty-title')!r}）")
        check(text_of("#badge") == "暂无待办", f"基线：徽标来自 JS（{text_of('#badge')!r}）")
        check(bool(client.ok("script", {"code": "typeof app.hot.reload"})["result"] == "function"),
              "宿主暴露了 app.hot.reload（脚本可一句话重载）")
        bindings0 = hot_bindings()

        # ① 改 XML → 界面重建
        write(xml_path, xml_backup.replace(">这里空着<", ">热重载标题<"))
        check(wait_for(lambda: text_of("#empty-title") == "热重载标题"),
              f"① 改 XML 即生效（{text_of('#empty-title')!r}）")
        check(text_of("#badge") == "暂无待办", "① 界面重建后逻辑自愈（徽标仍在）")
        # 新增元素也要在场（不是“把旧树原地改属性”，而是真重建）
        write(xml_path, xml_backup.replace('<Icon id="empty-icon"',
                                           '<Text id="hot-marker">新增标记</Text>\n        <Icon id="empty-icon"'))
        check(wait_for(lambda: bool(find("#hot-marker"))), "① 新增的 XML 元素出现在场")
        write(xml_path, xml_backup)
        check(wait_for(lambda: not find("#hot-marker")), "① 删掉 XML 元素 → 界面里真的没了")

        # ② 改 JS → 逻辑重装
        #
        # 判据三件：
        #   ⓐ 脚本装载时会打一行 log——把它改掉，日志里就该出现新文案（证明新脚本真跑了）；
        #   ⓑ 连续改两次仍然生效（不是“只认第一次”）；
        #   ⓒ **绑定数不随重载增长**（旧闭包还在时重载一次就多一套处理器——
        #      那是重载最常见的真实缺陷）。
        #
        # ⚪ 为何不看界面文案：相期的 `render()` 只在 `tick()` 的轮询成功后跑，
        #   而本用例不连控制器（poll 会抛）——界面文字由 C++ 默认值与脚本兜底分头写，
        #   拿它做“新脚本已跑”的判据会得到一个与被测事实无关的红灯。
        def log_text():
            return (shots / "log.txt").read_text(encoding="utf-8", errors="replace")

        write(js_path, js_backup.replace("log('相期就绪：等待 agent 投递卡片')",
                                         "log('HOT-一次：新脚本在跑')"))
        check(wait_for(lambda: "HOT-一次：新脚本在跑" in log_text()),
              "② 改 JS 即生效（新脚本的装载日志出现）")
        write(js_path, js_backup.replace("log('相期就绪：等待 agent 投递卡片')",
                                         "log('HOT-二次：再来一次')"))
        check(wait_for(lambda: "HOT-二次：再来一次" in log_text()),
              "② 连续重载仍然生效")
        check(hot_bindings() == bindings0,
              f"② 重载不堆绑定（{bindings0} → {hot_bindings()}）")
        write(js_path, js_backup)
        check(wait_for(lambda: log_text().count("相期就绪：等待 agent 投递卡片") >= 2),
              "② 改回原 JS 生效")

        # ③ 写坏 XML → 界面保持原样
        #
        # ⚠ 背景写清楚：坏 XML 时 `refresh_ui` **不会**动界面（原子替换）；但 `poll()`
        # 发现 XML 变了后会为了“重放绑定”再重装一次逻辑，而新逻辑跑的 `render()`
        # 找不到 `#badge`（新树不存在）——那是它自己把文案丢了，不是界面被清了。
        # 所以这里用**界面树自身的证据**：根元素还在、空态块还在、坏代码里的
        # `#oops` 没进来。
        write(xml_path, "<Column id=\"root\"><Buton id=\"oops\" label=\"拼错\"></Column>")
        time.sleep(1.2)
        check(bool(find("#root")) and bool(find("#empty")),
              "③ 坏 XML 不清界面（旧树仍在：root/empty 都在）")
        check(not find("#oops"), "③ 坏 XML 的新元素没被挂上（没有半棵新树）")
        still = (shots / "log.txt").read_text(encoding="utf-8", errors="replace")
        check("界面重载失败" in still, "③ 失败原因写进日志（界面重载失败）")
        write(xml_path, xml_backup)
        check(wait_for(lambda: text_of("#empty-title") == "这里空着"), "③ 改回来后自动恢复")

        # ④ 写坏 JS → 旧逻辑继续跑
        write(js_path, "function broken( { this is not js\n")
        time.sleep(1.2)
        check(find("#empty-title"), "④ 坏 JS 不影响正在跑的界面")
        client.ok("script", {"code": "$('#empty-title').set({text:'还活着'}); 0"})
        check(wait_for(lambda: text_of("#empty-title") == "还活着"),
              f"④ 旧脚本引擎仍可驱动界面（{text_of('#empty-title')!r}）")
        write(js_path, js_backup)
        check(wait_for(lambda: not (hot_status().get("last_error") or "")), "④ 改回来后再无异常")

        # ⑤ 诊断面：控制通道能问到“重载了几次、有没有错”
        status = hot_status()
        check(int(status.get("ui_refreshes") or 0) >= 4,
              f"⑤ app.hot.status() 可查（ui_refreshes={status.get('ui_refreshes')}）")
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
