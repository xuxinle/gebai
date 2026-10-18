#!/usr/bin/env python3
"""镂月（Lunaris）· **海报闭环 + 双向控制权**端到端验证。

覆盖「AI 主导做海报、人可随时抢回控制权」的可执行判据：

  1. 控制面存在：`#poster`（`LouyuePoster`）可寻址，属性面给出作品摘要；
  2. **人可拨开关**：`#poster-control-switch` 存在且可操作（不是只能由 AI 锁）；
  3. **脚本即真值**：`invoke apply_poster` 后块数/画布尺寸/图层数随之变；
  4. **坏脚本被拒且作品不变**（AI 写错不该毁掉作品）；
  5. **导出闭环**：`invoke export_png <path> [WxH]` → 文件存在、PNG 尺寸符合要求；
     不带尺寸时按**作品设计尺寸**（1920×1080）而不是当前画布尺寸；
  6. **AI 接管态**：写动作逐个被拒、查看类（pick/zoom/export）仍可用、
     解锁后恢复；
  7. **画布确实按视口渲染**：`#canvas` 的 `canvas_size` 不超过视口尺寸
     （否则 1920×1080 塞进小视口 → 只看得见一角，看着像"没内容"）。

用法: python3 tools/louyue_poster_e2e.py [binary] [shots]
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 默认用 **debug** 档：回归测试的规范档位是 debug（dev 是快速迭代档，
# 两者混用会出现「代码改了、测试还在跑旧二进制」的假象——实测踩过）。
# 需要其它档位时显式传参或设 ST_PROFILE。
_PROFILE = os.environ.get("ST_PROFILE", "debug")
DEFAULT_BIN = os.path.join(ROOT, "build", _PROFILE, "bin", "louyue.exe")

# 一份小海报：3 块，640×360 设计尺寸（渲染快）。
POSTER = {
    "name": "验证海报",
    "width": 640,
    "height": 360,
    "background": "#0a0f1e",
    "blocks": [
        {"kind": "background", "color": "#0a0f1e", "color_alt": "#1a2a4a"},
        {"kind": "title", "text": "镂月", "x": 0.5, "y": 96, "size": 72, "color": "#f5f8ff"},
        {"kind": "subtitle", "text": "LUNARIS", "x": 0.5, "y": 208, "size": 24,
         "tracking": 8, "color": "#aac6f0"},
    ],
}


class Client:
    def __init__(self, port, token):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.rid = 0
        self.token = token

    def call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
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
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def invoke(self, element_id, action, argument=None):
        params = {"id": element_id, "action": action}
        if argument is not None:
            params["argument"] = argument
        return self.ok("invoke", params).get("handled", False)

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]


def start(binary, shots):
    ctl = os.path.join(shots, "louyue-ctl.json")
    try:
        os.remove(ctl)
    except FileNotFoundError:
        pass
    log = open(os.path.join(shots, "louyue.log"), "wb")
    argv = [binary, "--headless", "--control-port", "0", "--control-file", ctl]
    process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(200):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl, encoding="utf-8"))
                if info.get("port"):
                    client = Client(info["port"], info.get("token", ""))
                    client.call("hello")   # 控制通道要求先握手（不带 token 的后续调用一律拒）
                    return process, client
            except Exception:
                pass
        time.sleep(0.2)
    raise RuntimeError("应用未在 40s 内写出控制文件")


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def png_size(path):
    """只读 PNG 头（不依赖 Pillow）：宽高在第 16..24 字节。"""
    with open(path, "rb") as handle:
        data = handle.read(33)
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssertionError(f"不是 PNG：{path}")
    width = int.from_bytes(data[16:20], "big")
    height = int.from_bytes(data[20:24], "big")
    return width, height


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    shots = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "tmp", "louyue-e2e")
    os.makedirs(shots, exist_ok=True)
    process, client = start(binary, shots)
    try:
        # —— 1. 控制面与作品摘要 ——
        check(client.count("#poster") == 1, "#poster 控制面不存在")
        info = client.props("poster")
        check(int(info["blocks"]) >= 3, f"内置示例海报块数过少：{info['blocks']}")
        check(info["name"], "作品名为空")
        print(f"[1] 控制面：#poster · {info['name']} · {info['size']} · {info['blocks']} 块")

        # —— 2. 人可拨开关（不是只能由 AI 锁）——
        check(client.count("#poster-control-switch") == 1,
              "界面上没有控制权开关——人无法自己切回主导")
        print("[2] 人可切换：控制权开关在界面上")

        # —— 3. 脚本即真值 ——
        check(client.invoke("poster", "apply_poster", json.dumps(POSTER)),
              "apply_poster 失败")
        time.sleep(0.3)
        after = client.props("poster")
        check(after["blocks"] == "3", f"块数没随脚本变：{after['blocks']}")
        check(after["name"] == POSTER["name"], f"作品名没变：{after['name']}")
        check(client.props("canvas")["canvas_size"].split("x")[0] != "0", "画布尺寸为 0")
        print(f"[3] 脚本即真值：{after['name']} · {after['size']} · {after['blocks']} 块")

        # —— 4. 坏脚本被拒且作品不变 ——
        before = client.props("poster")
        bad = json.dumps({"blocks": [{"kind": "不认识的块"}]})
        check(not client.invoke("poster", "apply_poster", bad), "坏脚本竟被接受")
        stable = client.props("poster")
        check(stable["blocks"] == before["blocks"], "坏脚本改动了作品")
        check(stable["name"] == before["name"], "坏脚本改动了作品名")
        print("[4] 坏脚本被拒，作品保持不变")

        # —— 5. 导出闭环 ——
        out1 = os.path.join(shots, "poster-default.png")
        check(client.invoke("poster", "export_png", out1), "export_png 失败（默认尺寸）")
        check(os.path.exists(out1), f"导出文件不存在：{out1}")
        w1, h1 = png_size(out1)
        # 不带尺寸 = 作品设计尺寸（640×360），不是当前画布（视口）尺寸
        check((w1, h1) == (POSTER["width"], POSTER["height"]),
              f"默认导出尺寸应为作品尺寸 {POSTER['width']}x{POSTER['height']}，实际 {w1}x{h1}")

        out2 = os.path.join(shots, "poster-4x3.png")
        check(client.invoke("poster", "export_png", f"{out2} 1280x720"), "export_png 失败（指定尺寸）")
        w2, h2 = png_size(out2)
        check((w2, h2) == (1280, 720), f"指定尺寸导出不符：{w2}x{h2}")
        print(f"[5] 导出闭环：默认 {w1}x{h1}（作品尺寸）、指定 {w2}x{h2}")

        # —— 6. AI 接管：写被拒、查可用、解锁恢复 ——
        check(client.invoke("poster", "edit_lock"), "edit_lock 失败")
        check(client.props("poster")["edit_locked"] == "true", "锁定后 edit_locked 不是 true")
        writes = [
            ("stroke", "10,10,60,60"),
            ("set_opacity", "0 0.3"),
            ("set_visible", "0 0"),
            ("add_layer", None),
            ("remove_layer", "0"),
            ("clear_layer", None),
            ("set_pixel", "10,10,#ff0000"),
        ]
        allowed = [name for name, arg in writes
                   if client.invoke("canvas", name, arg)]
        check(not allowed, f"AI 接管态下这些写动作没被拦住：{allowed}")
        # 查看类必须仍可用
        check(client.invoke("canvas", "pick", "100,100"), "接管态下 pick（查看）也被拦了")
        check(client.invoke("canvas", "zoom_out"), "接管态下 zoom（查看）也被拦了")
        print(f"[6] AI 接管：{len(writes)} 个写动作全被拒，查看仍可用")

        check(client.invoke("poster", "unlock_edit"), "unlock_edit 失败")
        check(client.props("poster")["edit_locked"] == "false", "解锁后仍为锁定")
        check(client.invoke("canvas", "set_opacity", "0 0.8"), "解锁后编辑仍被拒")
        print("[6b] 交还控制权：编辑恢复可用")

        # —— 7. 画布按视口渲染（不是 1920×1080 塞进小视口）——
        size = client.props("canvas")["canvas_size"]
        view = client.ok("find", {"selector": "#canvas-viewport"})["matches"][0]["bounds"]
        cw, ch = (int(v) for v in size.split("x"))
        check(cw <= view["width"] + 2 and ch <= view["height"] + 2,
              f"画布 {cw}x{ch} 超过视口 {view['width']}x{view['height']}——会只显示一角")
        print(f"[7] 画布按视口渲染：{cw}x{ch} ≤ 视口 {view['width']}x{view['height']}")

        # —— 8. 素材：图片块能用，且缺图时画占位框（不静默跳过）——
        #
        # 「静默跳过」比占位框糟：它会让"图没进来"看起来像"设计就是没图"。
        # 这条断言钉住的是那个占位行为本身。
        media = os.path.join(shots, "media")
        os.makedirs(media, exist_ok=True)
        asset = os.path.join(media, "probe.png")
        if not os.path.exists(asset):
            # 造一张 4x4 纯色 PNG（最小合法图，不依赖外部素材）
            import struct
            import zlib

            def chunk(kind, data):
                body = kind + data
                return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

            width = height = 4
            raw = b"".join(b"\x00" + bytes([40, 90, 200]) * width for _ in range(height))
            png = (b"\x89PNG\r\n\x1a\n"
                   + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                   + chunk(b"IDAT", zlib.compress(raw))
                   + chunk(b"IEND", b""))
            with open(asset, "wb") as handle:
                handle.write(png)

        poster_with_image = {
            "name": "素材验证",
            "width": 640,
            "height": 360,
            "background": "#0a0f1e",
            "blocks": [
                {"kind": "background", "color": "#0a0f1e"},
                {"kind": "image", "image": asset, "x": 0.5, "y": 60,
                 "width": 400, "height": 200, "radius": 12},
                {"kind": "title", "text": "素材", "x": 0.5, "y": 280, "size": 40,
                 "color": "#ffffff"},
            ],
        }
        check(client.invoke("poster", "apply_poster", json.dumps(poster_with_image)),
              "带 image 块的海报被拒")
        time.sleep(0.3)
        out3 = os.path.join(shots, "poster-with-image.png")
        check(client.invoke("poster", "export_png", out3), "带素材的海报导出失败")
        check(os.path.exists(out3), "带素材的海报文件不存在")
        check(png_size(out3) == (640, 360), f"带素材导出尺寸不对：{png_size(out3)}")

        # 缺图必须**画占位**（红色系），而不是画成正常底色 → 采样该区域看是否偏红
        missing = dict(poster_with_image)
        missing["blocks"] = [dict(poster_with_image["blocks"][0]),
                             dict(poster_with_image["blocks"][1], image="不存在的素材"),
                             dict(poster_with_image["blocks"][2])]
        check(client.invoke("poster", "apply_poster", json.dumps(missing)), "缺图海报被拒")
        time.sleep(0.3)
        out4 = os.path.join(shots, "poster-missing.png")
        check(client.invoke("poster", "export_png", out4), "缺图海报导出失败")
        try:
            from PIL import Image
            img = Image.open(out4).convert("RGB")
            # 图片块区域中心（设计坐标 640x360 → 同尺寸导出）
            px = img.getpixel((320, 160))
            reddish = px[0] > 40 and px[0] > px[2]
            check(reddish, f"缺图处没画占位框（采样 {px}，红色分量不突出）")
            print(f"[8] 素材：带图导出 {png_size(out3)}；缺图画占位（采样 {px}）")
        except ImportError:
            print(f"[8] 素材：带图导出 {png_size(out3)}（Pillow 缺失，跳过占位色采样）")

        # —— 9. 窄幅重排：并排的块在竖版/方版上必须改成堆叠，不能重叠 ——
        #
        # 判据是"文字区块之间不重叠"而不是"看起来对"：
        # 早期三块按设计基准的 x 比例并排，竖版变窄后直接撞成一串
        # （「任意语言任意进程收编为工具」），这是重排版问题，缩放救不了。
        flow_poster = {
            "name": "窄幅重排验证",
            "width": 1920,
            "height": 1080,
            "background": "#0a0f1e",
            "blocks": [
                {"kind": "background", "color": "#0a0f1e"},
                {"kind": "body", "text": "任意语言", "x": 0.22, "y": 700,
                 "size": 60, "flow": 1, "flow_rank": 0, "color": "#ffffff"},
                {"kind": "body", "text": "任意进程", "x": 0.50, "y": 700,
                 "size": 60, "flow": 1, "flow_rank": 1, "color": "#ffffff"},
                {"kind": "body", "text": "收编为工具", "x": 0.78, "y": 700,
                 "size": 60, "flow": 1, "flow_rank": 2, "color": "#ffffff"},
            ],
        }
        check(client.invoke("poster", "apply_poster", json.dumps(flow_poster)), "重排海报被拒")

        def ink_bands(path):
            """返回每段连续有墨迹的行区间（用极简的列扫描，不依赖连通域）。"""
            try:
                from PIL import Image
            except ImportError:
                return None
            img = Image.open(path).convert("RGB")
            w, h = img.size
            px = img.load()
            rows = []
            for y in range(int(h * 0.55), h):
                bg = px[4, y]
                hit = 0
                for x in range(20, w - 20, 2):
                    c = px[x, y]
                    if abs(c[0] - bg[0]) + abs(c[1] - bg[1]) + abs(c[2] - bg[2]) > 60:
                        hit += 1
                rows.append(hit > 3)
            bands = []
            start = None
            for i, filled in enumerate(rows):
                if filled and start is None:
                    start = i
                elif not filled and start is not None:
                    bands.append((start, i - 1))
                    start = None
            if start is not None:
                bands.append((start, len(rows) - 1))
            return [b for b in bands if b[1] - b[0] >= 3]

        # 横版：应在同一行（1 条墨迹带）
        wide = os.path.join(shots, "flow-wide.png")
        check(client.invoke("poster", "export_png", f"{wide} 1920x1080"), "横版重排导出失败")
        # 竖版：应堆叠成 3 条（互不重叠）
        tall = os.path.join(shots, "flow-tall.png")
        check(client.invoke("poster", "export_png", f"{tall} 1080x1920"), "竖版重排导出失败")
        wide_bands = ink_bands(wide)
        tall_bands = ink_bands(tall)
        if wide_bands is not None and tall_bands is not None:
            check(len(wide_bands) == 1,
                  f"横版三段应当并排成 1 条墨迹带，实测 {len(wide_bands)} 条 {wide_bands}")
            check(len(tall_bands) >= 3,
                  f"竖版三段应当堆叠成 ≥3 条墨迹带，实测 {len(tall_bands)} 条 {tall_bands}")
            print(f"[9] 窄幅重排：横版 {len(wide_bands)} 条墨迹带 / 竖版 {len(tall_bands)} 条（无重叠）")
        else:
            print("[9] 窄幅重排：Pillow 缺失，跳过墨迹带采样（导出成功已验）")

        # —— 10. 通用图元：line/path/rect/paragraph 能落位且 JSON 往返不丢字段 ——
        #
        # 判据分两层：
        #  ① 几何存在（导出图在这些位置真有墨迹）——防"字段解析了但没画"；
        #  ② JSON 往返后字段还在——防"画出来了但序列化丢了"（用户存盘再打开就没了）。
        prim_poster = {
            "name": "图元验证", "width": 1920, "height": 1080, "background": "#070c18",
            "blocks": [
                {"kind": "background", "color": "#070c18"},
                # 斜线：横跨画面中上部，端点明确
                {"kind": "line", "x1": 300, "y1": 300, "x2": 900, "y2": 500,
                 "color": "#5ee0c0", "thickness": 6},
                # 实心多边形
                {"kind": "path", "points": [1100, 300, 1300, 300, 1250, 480, 1150, 480],
                 "color": "#e0a050", "filled": True},
                # 只描边折线
                {"kind": "path", "points": [300, 700, 500, 800, 700, 700],
                 "color": "#7ab4ff", "filled": False, "thickness": 5},
                # 圆角矩形
                {"kind": "rect", "x": 0.5, "y": 200, "box_w": 300, "box_h": 160,
                 "corner": 20, "color": "#1e3a5f"},
                # 左对齐多行段落（Body 做不到的那个能力）
                {"kind": "paragraph", "text": "段落第一行\n段落第二行",
                 "x": 0.12, "y": 880, "size": 48, "color": "#ffffff", "align": 1,
                 "line_height": 1.6},
            ],
        }
        check(client.invoke("poster", "apply_poster", json.dumps(prim_poster)), "图元海报被拒")
        time.sleep(0.3)
        prim_info = client.props("poster")
        prim_png = os.path.join(shots, "poster-primitives.png")
        check(client.invoke("poster", "export_png", f"{prim_png} 1920x1080"), "图元导出失败")
        check(os.path.exists(prim_png), "图元 PNG 不存在")

        # ① 几何存在：在每种图元的预期位置采样，确认有非底色墨迹
        try:
            from PIL import Image
            img = Image.open(prim_png).convert("RGB")
            px = img.load()
            bg = px[4, 4]

            def inked(cx, cy, radius=14):
                """(cx,cy) 周围是否有与底色明显不同的像素。"""
                for y in range(max(0, cy - radius), min(img.size[1], cy + radius)):
                    for x in range(max(0, cx - radius), min(img.size[0], cx + radius)):
                        c = px[x, y]
                        if abs(c[0] - bg[0]) + abs(c[1] - bg[1]) + abs(c[2] - bg[2]) > 40:
                            return True
                return False

            check(inked(600, 400), "斜线位置没有墨迹（line 未生效）")
            check(inked(1200, 390), "多边形内部没有墨迹（path 填充未生效）")
            check(inked(500, 800), "折线顶点没有墨迹（path 描边未生效）")
            check(inked(960, 280), "圆角矩形位置没有墨迹（rect 未生效）")
            check(inked(400, 900), "左对齐段落位置没有墨迹（paragraph 未生效）")
            # 段落两行都要有字（只画一行 = 换行没生效）
            check(inked(400, 980, 18), "段落第二行没有墨迹（换行未生效）")
            print("[10] 通用图元：line/path(填充+描边)/rect/paragraph 全部落位")
        except ImportError:
            print("[10] 通用图元：Pillow 缺失，跳过几何采样（导出成功已验）")

        # ② JSON 往返：重新读回属性，确认图元字段还在（用户存盘再开不能丢）
        roundtrip = json.loads(prim_info["poster"])
        kinds = [b.get("kind") for b in roundtrip["blocks"]]
        for needed in ("line", "path", "rect", "paragraph"):
            check(needed in kinds, f"JSON 往返后丢了 {needed} 块（序列化不完整）")
        line_block = next(b for b in roundtrip["blocks"] if b.get("kind") == "line")
        check("x1" in line_block and "y2" in line_block, "line 的端点字段往返后丢失")
        path_block = next(b for b in roundtrip["blocks"] if b.get("kind") == "path")
        check("points" in path_block and len(path_block["points"]) >= 4,
              "path 的 points 往返后丢失")
        para_block = next(b for b in roundtrip["blocks"] if b.get("kind") == "paragraph")
        check("line_height" in para_block, "paragraph 的 line_height 往返后丢失")
        print("[10] 通用图元：JSON 往返字段完整（x1/y2/points/line_height）")

        print("\n[OK] 镂月海报闭环 + 双向控制权 + 素材 + 窄幅重排 + 通用图元 全部通过")
        return 0
    finally:
        try:
            client.call("shutdown")
        except Exception:
            pass
        try:
            process.wait(timeout=10)
        except Exception:
            process.kill()


if __name__ == "__main__":
    sys.exit(main())
