"""焦点环「只在键盘导航时出现」的 E2E 回归（区域像素断言）。

跑法：py -3 tools/gbcode_focus_ring_e2e.py

用户报的现象：活动栏按钮**点一下就带一圈边框**。
根因：鼠标按下可聚焦元素也会给焦点，而 Button 只要 `focused_` 就画焦点环。
修法：宿主记「焦点环是否该画」（`UiRoot::focus_ring_visible`）——鼠标按下置假、
键盘事件置真；组件画环前问宿主。

判据是**像素**（环是画出来的东西，不看内部字段），两条：

① **点击不留环**：先把视图切回原点，再对照同一按钮区域——若点击留了环，
   该区域会与「从未点过」的截图不同。用「切走再切回」保证两张图处在**同一视图
   状态**（否则选中态本身就会变，对照就不成立）。
② **键盘仍画环**：Tab 之后按钮区域必须出现可测差异——证明环的绘制路径没被修死。
"""

import base64
import json
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageChops  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / "gbcode.exe"


def log(msg):
    print(msg, flush=True)


class Client:
    def __init__(self, port, token):
        self.port = port
        self.token = token
        self.rid = 0
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)

    def _read(self, size):
        buf = b""
        while len(buf) < size:
            chunk = self.sock.recv(size - len(buf))
            if not chunk:
                raise RuntimeError("控制通道提前关闭")
            buf += chunk
        return buf

    def _call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        return json.loads(self._read(struct.unpack(">I", self._read(4))[0]))

    def call(self, method, params=None):
        try:
            return self._call(method, params)
        except (ConnectionAbortedError, ConnectionResetError):
            self.sock.close()
            self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=30.0)
            self.sock.settimeout(30.0)
            self.rid = 0
            self._call("hello")
            return self._call(method, params)

    def ok(self, method, params=None):
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def bounds(self, selector):
        matches = self.ok("find", {"selector": selector}).get("matches") or []
        return matches[0]["bounds"] if matches else None

    def click(self, selector):
        box = self.bounds(selector)
        if box is None:
            raise AssertionError(f"找不到元素 {selector}")
        self.ok("input.mouse", {"kind": "click", "x": box["x"] + box["width"] / 2.0,
                                "y": box["y"] + box["height"] / 2.0, "button": 1})
        time.sleep(0.45)

    def park_pointer(self, x=640.0, y=400.0):
        """把指针移到中性位置（编辑器中央）。

        ⚠ 截图对照前**必须**先把指针移开：点完按钮后鼠标就停在它上面，
        hover 高亮会让区域与基准不同（实测差 1.37，而阈值是 1.0）——
        那是悬停态，不是焦点环；不区分会把“悬停”误判成“环没消失”。
        """
        self.ok("input.mouse", {"kind": "move", "x": x, "y": y})
        time.sleep(0.8)   # 等 hover 过渡收敛

    def screenshot(self, path):
        pathlib.Path(path).write_bytes(base64.b64decode(self.ok("capture", {})["base64"]))
        return pathlib.Path(path)


def region_of(image_path, box, pad=5.0):
    """按**逻辑坐标**裁一块（截图是物理像素；二者比值由图像宽度/视口宽度决定）。"""
    image = Image.open(image_path).convert("RGBA")
    scale = image.width / 1280.0   # 无头视口固定 1280 逻辑宽
    left = int((box["x"] - pad) * scale)
    top = int((box["y"] - pad) * scale)
    right = int((box["x"] + box["width"] + pad) * scale)
    bottom = int((box["y"] + box["height"] + pad) * scale)
    return image.crop((max(0, left), max(0, top), right, bottom))


def region_diff(a, b):
    """两块的**平均绝对差**（0..255）。"""
    if a.size != b.size:
        return 999.0
    diff = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
    histogram = diff.histogram()
    total = 0
    count = 0
    for channel in range(3):
        for value in range(256):
            hits = histogram[channel * 256 + value]
            total += value * hits
            count += hits
    return total / max(1, count)


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}")
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_focus_"))
    repo = work / "repo"
    repo.mkdir()
    subprocess.run(["git", "init", "-q", "-b", "main"], cwd=repo, capture_output=True)
    (repo / "a.txt").write_text("x\n", encoding="utf-8")

    failures = []

    def check(ok, label):
        log(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    proc = None
    try:
        shots = ROOT / "tmp" / "gbcode-focus-shots"
        shots.mkdir(parents=True, exist_ok=True)
        ctl = shots / "gbcode-focus-ctl.json"
        ctl.unlink(missing_ok=True)
        handle = open(shots / "gbcode-focus.log", "wb")
        proc = subprocess.Popen(
            [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
             "--shots", str(shots), "--tool-root", str(ROOT), "--workspace", str(repo)],
            stdout=handle, stderr=subprocess.STDOUT)
        client = None
        for _ in range(160):
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
            raise AssertionError("控制通道没起来")

        # 基准：资源管理器视图 + 焦点在编辑器 + **指针在中性位**
        #（不把指针停在任何按钮上——否则 hover 高亮会混进对照）。
        client.ok("invoke", {"id": "editor", "action": "focus"})
        client.park_pointer()
        time.sleep(0.4)
        base = client.screenshot(shots / "ring-base.png")
        explorer_box = client.bounds("#activity-explorer")
        search_box = client.bounds("#activity-search")
        check(explorer_box is not None and search_box is not None, "活动栏按钮有几何")

        ref_explorer = region_of(base, explorer_box)

        # ① 点击「搜索」再切回「资源管理器」——同一视图状态，对照同一按钮区域。
        client.click("#activity-search")
        client.click("#activity-explorer")
        client.park_pointer()
        clicked = client.screenshot(shots / "ring-after-click.png")
        after_explorer = region_of(clicked, explorer_box)
        diff_click = region_diff(ref_explorer, after_explorer)
        log(f"  点击后「资源管理器」按钮区域平均差 = {diff_click:.3f}")
        check(diff_click < 1.0,
              f"点击**不留焦点环**（区域平均差 {diff_click:.3f} < 1.0）")

        # ② 键盘 Tab：焦点环应当出现（活动栏区域内出现可测差异）。
        #    先确认**没焦点**（否则 Tab 只是在已有焦点上跳，看不出“环出现”）。
        client.ok("set", {"id": "editor", "props": {"read_only": "false"}})
        client.ok("input.key", {"key": "Escape", "kind": "press"})
        time.sleep(0.3)
        for _ in range(3):
            client.ok("input.key", {"key": "Tab", "kind": "press"})
            time.sleep(0.25)
        time.sleep(0.5)
        keyed = client.screenshot(shots / "ring-after-key.png")
        bar_box = client.bounds("#activity-bar")
        ref_bar = region_of(base, bar_box, pad=0.0)
        key_bar = region_of(keyed, bar_box, pad=0.0)
        diff_key = region_diff(ref_bar, key_bar)
        log(f"  键盘 Tab 后活动栏区域平均差 = {diff_key:.3f}")
        check(diff_key > 0.05,
              f"键盘导航**仍画焦点环**（区域平均差 {diff_key:.3f} > 0.05）")

        # ③ 键盘出现环之后，**点一下鼠标**——环应当消失（回到与基准一致）。
        #
        # ⚠ 不要点「当前视图那个按钮」来验证：那一下是**收起侧栏**（交互词形），
        # 侧栏消失会改变活动栏右缘那一列像素（实测 bbox 宽 7px），看上去像
        # “环没消失”。路径选“点另一个视图再点回来”，焦点归位且侧栏仍开着。
        client.click("#activity-search")
        client.click("#activity-explorer")
        client.park_pointer()
        reclicked = client.screenshot(shots / "ring-after-reclick.png")
        back_explorer = region_of(reclicked, explorer_box)
        diff_back = region_diff(ref_explorer, back_explorer)
        log(f"  键盘出现环后点击「资源管理器」区域平均差 = {diff_back:.3f}")
        check(diff_back < 1.0, f"鼠标点击后焦点环消失（区域平均差 {diff_back:.3f} < 1.0）")
    finally:
        if proc is not None:
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
