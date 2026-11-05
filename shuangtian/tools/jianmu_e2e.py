#!/usr/bin/env python3
"""建木（jianmu）端到端验收：**热重载 + 交互 + 拾取**三件事真的成立吗。

覆盖两种脚本表达层：`.js`（three.js）与 `.py`（bpy）——后者是本应用新增的
那半边，判据必须能证明“py 场景真的被装载并画出了东西”，而不只是“没报错”。

## 这个脚本要证的四件事（每件都能独立失败）

1. **脚本渲染出图**：视口里真的有三维内容（不是空场景）。
   判据：截图里"非背景像素"占一定比例——不是"进程没崩"。
2. **热重载生效**：改 `.js` 文件后，画面**真的变了**。
   判据：改前/改后的截图像素差异超过阈值。
   这条最容易假绿：如果不改文件也"变了"（比如有旋转动画），
   那判据就废了。所以先拍两张**不改文件**的作为基线噪声，再比改后的差异。
3. **相机交互生效**：缩放与旋转改变画面。
   判据：`lab.*` 暴露的 `distance`/`yaw` 真的变了，且画面随之变化。
4. **拾取生效**：把指针移到物体上会命中（`hovered` 非空、`hover_events` 增长）。
   判据：命中到**具体物体名**，而不是"有反应"。

## 为什么用控制通道而不是注入真实鼠标

注入鼠标要算屏幕坐标，而窗口位置/DPI/装饰在不同机器上都不同——那种用例
在 CI 上会偶发失败，而失败原因与产品无关。控制通道的 `events` 走的是
**同一套事件分发**（UiRoot 的命中测试与 `on_event`），只是省掉了坐标换算。
所以它验的是真行为，不是"另一条假路径"。
"""

import json
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "build", "dev", "bin", "jianmu.exe")
SHOTS = os.path.join(ROOT, "build", "e2e-jianmu")


class Client:
    def __init__(self, port, token):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.rid = 0
        self.token = token

    def call(self, method, params=None):
        self.rid += 1
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        header = b""
        while len(header) < 4:
            chunk = self.sock.recv(4 - len(header))
            if not chunk:
                raise RuntimeError("控制通道在响应头前关闭")
            header += chunk
        length = struct.unpack(">I", header)[0]
        payload = b""
        while len(payload) < length:
            chunk = self.sock.recv(length - len(payload))
            if not chunk:
                raise RuntimeError("控制通道在响应体前关闭")
            payload += chunk
        return json.loads(payload)

    def ok(self, method, params=None):
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise RuntimeError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result", {})

    def hello(self):
        return self.ok("hello", {"token": self.token})


def prop_of(client, element_id, name):
    """读一个元素属性。

    ⚠ 两个坑，都是实测踩到的：

    1. 属性在返回快照的 **`props`** 里，不在顶层。第一版直接读 `result["value"]`
       得到空字符串，于是“物体数 0”让我误以为 three 场景没加载——而同一轮的
       截图覆盖率是 93.5%（明显已经画了）。**假阴性比假阳性更难查**：
       它会让你去查一个根本没坏的东西。
    2. `hovered` 是**框架级**属性（“指针是否悬停在本元素上”的布尔值），
       与三维视口自己的“悬浮到哪个物体”撞名。视口那边已改名为 `picked`。
    """
    reply = client.ok("get", {"id": element_id})
    return reply.get("props", {}).get(name, "")


def viewport_center(client):
    """视口中心（**从快照读实际 bounds**，不猜坐标）。

    ## 为什么不能写死坐标

    第一版把点击/滚轮坐标硬编码成 `(480, 320)`。而视口实际的 bounds 是
    `y=40, height=320`（上面还有标题栏）——于是 `y=320` 正好落在**视口下沿之外**，
    所有滚轮/悬浮/点击都被判为“不在视口内”。

    症状是假阴性：`handled=true`（事件确实被处理了，只是被当成“视口外”）、
    “滚轮不缩放”、“悬浮不命中任何物体”。把坐标改成从 bounds 算，
    这个类的错就不可能再出现。
    """
    reply = client.ok("get", {"id": "viewport"})
    box = reply.get("bounds", {})
    return (box.get("x", 0.0) + box.get("width", 0.0) / 2.0,
            box.get("y", 0.0) + box.get("height", 0.0) / 2.0,
            box)


def start(binary):
    os.makedirs(SHOTS, exist_ok=True)
    ctl = os.path.join(SHOTS, "jianmu-ctl.json")
    try:
        os.remove(ctl)
    except FileNotFoundError:
        pass
    log_path = os.path.join(SHOTS, "jianmu.log")
    log = open(log_path, "wb")
    argv = [binary, "--headless", "--control-port", "0", "--control-file", ctl,
            "--width", "960", "--height", "640"]
    process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
    for _ in range(300):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl, encoding="utf-8"))
                if info.get("port"):
                    return process, Client(info["port"], info.get("token", "")), log_path
            except Exception:
                pass
        if process.poll() is not None:
            raise RuntimeError(f"应用提前退出（退出码 {process.returncode}），日志 {log_path}")
        time.sleep(0.2)
    raise RuntimeError("应用未在 60s 内写出控制文件")


def decode_png(path):
    """读 PNG 成 RGBA 字节（只用标准库；够本脚本用）。

    需要 zlib 解压 + 反滤波。写得短但完整——本仓的判据不能依赖外部图像库。
    """
    import zlib
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError("不是 PNG")
    pos = 8
    width = height = None
    bit_depth = color_type = None
    idat = bytearray()
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", payload[:10])
        elif kind == b"IDAT":
            idat += payload
        elif kind == b"IEND":
            break
    if bit_depth != 8 or color_type not in (2, 6):
        raise RuntimeError(f"不支持的 PNG 形态：depth={bit_depth} color={color_type}")
    channels = 4 if color_type == 6 else 3
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * channels)
    prev = bytearray(stride)
    offset = 0
    for row in range(height):
        filter_type = raw[offset]
        offset += 1
        line = bytearray(raw[offset:offset + stride])
        offset += stride
        if filter_type == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[row * stride:(row + 1) * stride] = line
        prev = line
    return width, height, channels, bytes(out)


def pixel_diff_ratio(path_a, path_b):
    """两张图的像素差异比例（0..1）。"""
    wa, ha, ca, pa = decode_png(path_a)
    wb, hb, cb, pb = decode_png(path_b)
    if (wa, ha) != (wb, hb):
        return 1.0
    total = wa * ha
    different = 0
    step = ca
    for i in range(total):
        base = i * step
        if ca != cb:
            return 1.0
        if (pa[base] != pb[base] or pa[base + 1] != pb[base + 1]
                or pa[base + 2] != pb[base + 2]):
            different += 1
    return different / total


def painted_ratio(path):
    """非背景像素比例：以左上角像素为背景（与 `scene_report` 同一口径）。"""
    width, height, channels, pixels = decode_png(path)
    bg = pixels[0:3]
    total = width * height
    painted = 0
    for i in range(total):
        base = i * channels
        dist = (abs(pixels[base] - bg[0]) + abs(pixels[base + 1] - bg[1])
                + abs(pixels[base + 2] - bg[2]))
        if dist > 12:
            painted += 1
    return painted / total


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    if not os.path.exists(binary):
        print(f"找不到可执行文件：{binary}")
        return 2

    failures = []

    def check(name, ok, detail=""):
        print(f"  [{'PASS' if ok else 'FAIL'}] {name:<44} {detail}")
        if not ok:
            failures.append(name)

    process = None
    original_source = None
    scene_path = os.path.join(ROOT, "examples", "jianmu", "scenes", "01-product.js")
    try:
        process, client, log_path = start(binary)
        client.hello()

        # ## 先切到被测场景
        #
        # 应用默认打开 04-watch（最能体现能力的那个），而热重载那几条改的是
        # 01-product.js。不切的话“改了文件但 reloads 不涨”——因为监听的是
        # **当前显示的那个**文件。这个失败看起来像“热重载坏了”，
        # 实际只是测错了对象。
        client.ok("script", {"code": "app.lab.select(0)"})
        time.sleep(0.6)

        # ── ① 启动即有图 ──
        time.sleep(0.8)
        shot_a = os.path.join(SHOTS, "01-initial.png")
        client.ok("capture", {"encode": "file", "path": shot_a})
        painted = painted_ratio(shot_a)
        check("视口画出内容（覆盖 > 5%）", painted > 0.05, f"{painted * 100:.1f}%")

        # ── ② 状态面可查 ──
        objects = int(prop_of(client, "viewport", "objects") or "0")
        # 01-product.js 就是 3 个物体（球/盒/底座），判据只要“> 0 且与文本一致”
        check("视口报告物体数", objects >= 3, f"{objects} 个")
        backend = prop_of(client, "viewport", "backend")
        check("视口能报后端", backend in ("gpu", "software"), backend)

        # ── ③ 基线噪声：不改文件，连拍两张 ──
        #
        # 这一步是防止"热重载判据"被动画骗过。本场景是静态的，
        # 所以两张应当几乎相同；若它们差异很大，说明有东西在动，
        # 那后面的"改了之后差异"就证明不了任何事。
        shot_b = os.path.join(SHOTS, "02-before-edit.png")
        time.sleep(0.35)
        client.ok("capture", {"encode": "file", "path": shot_b})
        baseline = pixel_diff_ratio(shot_a, shot_b)
        check("静态场景无自变动（基线噪声 < 2%）", baseline < 0.02, f"{baseline * 100:.2f}%")

        # ── ④ 热重载：改文件 → 画面必须变 ──
        original_source = open(scene_path, encoding="utf-8").read()
        # 把球改大、换色 —— 任何一项都足以让画面明显不同
        edited = original_source.replace("0.62, 48, 32", "0.95, 48, 32").replace(
            "0xd09050", "0x50d090")
        with open(scene_path, "w", encoding="utf-8") as handle:
            handle.write(edited)
        # 文件监听是轮询，给它几轮
        deadline = time.time() + 8.0
        reloaded = False
        while time.time() < deadline:
            # 桥暴露的名字带 `app.` 前缀（`expose("lab.status")` → `app.lab.status()`）
            status = client.ok("script", {"code": "app.lab.status()"})
            value = status.get("result") or {}
            if isinstance(value, dict) and value.get("reloads", 0) > 1:
                reloaded = True
                break
            time.sleep(0.25)
        check("热重载被触发（reloads 增长）", reloaded,
              f"reloads={value.get('reloads') if isinstance(value, dict) else '?'}")

        time.sleep(0.4)
        shot_c = os.path.join(SHOTS, "03-after-edit.png")
        client.ok("capture", {"encode": "file", "path": shot_c})
        changed = pixel_diff_ratio(shot_b, shot_c)
        # 判据要比基线噪声**明显**大（不是"大于 0"）
        check("改文件后画面变化（> 基线噪声 5 倍）", changed > max(baseline * 5, 0.03),
              f"{changed * 100:.1f}% vs 基线 {baseline * 100:.2f}%")

        # ── ⑤ 相机交互：缩放 ──
        before_distance = float(prop_of(client, "viewport", "distance") or "0")
        client.ok("invoke", {"id": "viewport", "action": "reset_view"})
        time.sleep(0.15)
        home = float(prop_of(client, "viewport", "distance") or "0")
        # 坐标**从 bounds 算**（写死像素会在视口尺寸变化时默默失效）
        cx, cy, vbox = viewport_center(client)
        client.ok("input.mouse", {"kind": "wheel", "x": cx, "y": cy, "delta": 120})
        time.sleep(0.2)
        after_distance = float(prop_of(client, "viewport", "distance") or "0")
        check("滚轮缩放改变相机距离", abs(after_distance - home) > 0.05,
              f"{home:.3f} → {after_distance:.3f}（初值 {before_distance:.3f}）")

        shot_d = os.path.join(SHOTS, "04-zoomed.png")
        client.ok("capture", {"encode": "file", "path": shot_d})
        zoom_change = pixel_diff_ratio(shot_c, shot_d)
        check("缩放后画面随之变化（> 2%）", zoom_change > 0.02, f"{zoom_change * 100:.1f}%")

        # ── ⑥ 相机交互：拖拽旋转 ──
        client.ok("invoke", {"id": "viewport", "action": "reset_view"})
        time.sleep(0.15)
        yaw_before = float(prop_of(client, "viewport", "yaw") or "0")
        # `drag` 合成 按下 → 移动 → 松开（真实的拖拽分发路径）
        cx, cy, _ = viewport_center(client)
        client.ok("input.mouse", {"kind": "drag", "x": cx, "y": cy,
                                  "to_x": cx + vbox.get("width", 600) * 0.25,
                                  "to_y": cy, "button": 0})
        time.sleep(0.25)
        yaw_after = float(prop_of(client, "viewport", "yaw") or "0")
        check("拖拽改变相机偏航", abs(yaw_after - yaw_before) > 0.1,
              f"{yaw_before:.3f} → {yaw_after:.3f}")

        # ── ⑦ 拾取：在视口内按比例扫点，找出真的能命中的位置 ──
        client.ok("invoke", {"id": "viewport", "action": "reset_view"})
        time.sleep(0.15)
        # 先把指针移到视口外，清掉上一轮遗留的悬浮态——
        # 否则“第一个扫点就命中同一物体”时计数不会增长，而那不是缺陷。
        client.ok("input.mouse", {"kind": "leave"})
        time.sleep(0.15)
        hover_before = int(prop_of(client, "viewport", "hover_events") or "0")
        hit_name = ""
        cx, cy, vbox = viewport_center(client)
        hit_point = (cx, cy)
        offsets = [(0.0, 0.05), (-0.10, 0.05), (0.10, 0.05), (0.0, -0.12),
                   (-0.05, 0.15), (0.05, 0.08), (-0.18, -0.04), (0.18, -0.04)]
        for dx, dy in offsets:
            x = cx + dx * vbox.get("width", 1.0)
            y = cy + dy * vbox.get("height", 1.0)
            client.ok("input.mouse", {"kind": "move", "x": x, "y": y})
            time.sleep(0.12)
            value = prop_of(client, "viewport", "picked")
            if value:
                hit_name = value
                hit_point = (x, y)
                break
        hover_after = int(prop_of(client, "viewport", "hover_events") or "0")
        check("悬浮命中到具体物体", bool(hit_name), hit_name or f"（扫了 {len(offsets)} 个点都没命中）")
        check("悬浮事件计数增长", hover_after > hover_before,
              f"{hover_before} → {hover_after}")

        # ── ⑧ 点击选中 ──
        click_before = int(prop_of(client, "viewport", "click_events") or "0")
        # 属性名是 `selected_name`（不是 `selected`）：
        # `selected` 容易与框架将来的“多选/焦点”语义撞名，所以三选一加了后缀。
        # 点之前再移一次到同一位置：确保 press 与 release 的坐标一致（否则被当成拖拽）
        client.ok("input.mouse", {"kind": "move", "x": hit_point[0], "y": hit_point[1]})
        time.sleep(0.1)
        client.ok("input.mouse", {"kind": "click", "x": hit_point[0], "y": hit_point[1],
                                  "button": 0})
        time.sleep(0.3)
        click_after = int(prop_of(client, "viewport", "click_events") or "0")
        selected = prop_of(client, "viewport", "selected_name")
        check("点击事件计数增长", click_after > click_before,
              f"{click_before} → {click_after}")
        check("点击选中了具体物体", bool(selected), selected or "（未选中）")
        if hit_name:
            check("选中的就是悬浮命中的那个", selected == hit_name,
                  f"picked={hit_name} selected={selected}")

        # ── ⑨ 脚本写错时：不清空、错误可见 ──
        with open(scene_path, "w", encoding="utf-8") as handle:
            handle.write("const scene = new THREE.Scene(;   // ← 故意的语法错误\n")
        deadline = time.time() + 8.0
        got_error = False
        while time.time() < deadline:
            value = prop_of(client, "viewport", "error")
            if value:
                got_error = True
                break
            time.sleep(0.25)
        check("脚本错误被捕获并暴露", got_error, (value or "")[:60])
        # 关键：错误时**画面不该变空**（保留上一个可用场景）
        shot_e = os.path.join(SHOTS, "05-after-broken.png")
        client.ok("capture", {"encode": "file", "path": shot_e})
        still_painted = painted_ratio(shot_e)
        check("脚本写坏后画面仍在（不清空）", still_painted > 0.05,
              f"{still_painted * 100:.1f}%")

        # ── ⑩ 修回来 ──
        with open(scene_path, "w", encoding="utf-8") as handle:
            handle.write(original_source)
        deadline = time.time() + 8.0
        recovered = False
        while time.time() < deadline:
            value = prop_of(client, "viewport", "error")
            if not value:
                recovered = True
                break
            time.sleep(0.25)
        check("修回脚本后错误清除", recovered)

        # ── ⑪ bpy 场景（.py）：同一条视口能力面必须都成立 ──
        #
        # 这一节直接检验本应用新加的那半边：扩展名 `.py` → `BpySceneExtractor`。
        # 判据要点是“真装进去了并且看得见”——只断言“没报错”会被“默默没装”骗过
        #（版本兼容/兜底默认都能让它看起来正常）。
        py_scene = os.path.join(ROOT, "examples", "jianmu", "scenes", "quad.py")
        py_original = open(py_scene, encoding="utf-8").read()
        try:
            # 切到 bpy 场景（下标由 `lab.scenes` 清单决定，不能写死：
            # 目录里有多少个 .js/.py 是会变的）
            scenes = client.ok("script", {"code": "app.lab.scenes()"}).get("result") or []
            # ⚠ 判据用**带扩展名的文件名**（`quad.py`）：清单里同一场景
            # 常常有两种写法并存（`quad.js` / `quad.py`），裸主名匹配会选错。
            py_index = next((i for i, item in enumerate(scenes)
                             if str(item.get("name", "")) == "quad.py"), -1)
            check("场景清单里包含 .py 脚本", py_index >= 0,
                  f"清单 {len(scenes)} 项：{[s.get('name') for s in scenes][:6]}…")
            if py_index < 0:
                raise RuntimeError("没有 .py 场景可测")
            client.ok("script", {"code": f"app.lab.select({py_index})"})
            time.sleep(2.2)   # bpy 要跑 MicroPython + 抽取，比 js 慢一个数量级
            py_error = prop_of(client, "viewport", "error")
            check("bpy 场景装载无错", not py_error, str(py_error)[:70])
            objects_py = int(prop_of(client, "viewport", "objects") or "0")
            check("bpy 场景有物体", objects_py >= 1, f"{objects_py} 个")
            py_shot = os.path.join(SHOTS, "06-bpy.png")
            client.ok("capture", {"encode": "file", "path": py_shot})
            py_painted = painted_ratio(py_shot)
            check("bpy 场景画出内容（覆盖 > 3%）", py_painted > 0.03,
                  f"{py_painted * 100:.1f}%")

            # bpy 也走热重载：改 .py → reloads 增长 + 画面变
            py_shot_b = os.path.join(SHOTS, "07-bpy-before-edit.png")
            time.sleep(0.4)
            client.ok("capture", {"encode": "file", "path": py_shot_b})
            py_baseline = pixel_diff_ratio(py_shot, py_shot_b)
            py_reloads0 = 0
            _status = client.ok("script", {"code": "app.lab.status()"}).get("result") or {}
            if isinstance(_status, dict):
                py_reloads0 = int(_status.get("reloads") or 0)
            with open(py_scene, "w", encoding="utf-8") as handle:
                handle.write(py_original.replace("(0.55, 0.62, 0.75), 0.00, 0.50",
                                                "(0.85, 0.20, 0.95), 0.00, 0.50"))
            # 先等“重载真的发生了”（py 要跑 MicroPython + 抽取，比 js 慢一个量级），
            # 再截图——只 sleep 固定时长会把“慢”测成“没生效”。
            py_reloads_before = int(prop_of(client, "viewport", "objects") or "0")
            deadline = time.time() + 12.0
            while time.time() < deadline:
                value = client.ok("script", {"code": "app.lab.status()"}).get("result") or {}
                if isinstance(value, dict) and value.get("reloads", 0) >= py_reloads0 + 1:
                    break
                time.sleep(0.25)
            time.sleep(0.5)
            py_shot_c = os.path.join(SHOTS, "08-bpy-after-edit.png")
            client.ok("capture", {"encode": "file", "path": py_shot_c})
            py_changed = pixel_diff_ratio(py_shot_b, py_shot_c)
            check("改 .py 后画面变化（> 基线噪声 3 倍）",
                  py_changed > max(py_baseline * 3, 0.01),
                  f"{py_changed * 100:.1f}% vs 基线 {py_baseline * 100:.2f}%")
        finally:
            with open(py_scene, "w", encoding="utf-8") as handle:
                handle.write(py_original)

        try:
            client.ok("shutdown")
        except Exception:
            pass
    finally:
        # **无论如何都要恢复场景文件**：这是仓库里的示例素材，改坏下一个用例会连带红
        if original_source is not None:
            with open(scene_path, "w", encoding="utf-8") as handle:
                handle.write(original_source)
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()

    print()
    if failures:
        print(f"失败 {len(failures)} 项：{', '.join(failures)}")
        return 1
    print("建木 e2e 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
