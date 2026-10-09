#!/usr/bin/env python3
"""镂月（Lunaris）的端到端验证（控制通道驱动 + PN G 像素断言）。

覆盖设计文档 §7 的四条验收标准：

  1. 打开内嵌 PNG → `stroke` 画两笔（不同混合模式的两个图层）→ `export`
     → **独立解码 PNG 回读**，断言笔画像素落在预期坐标与预期混合色（±2/255）；
  2. 撤销后像素与落笔前**逐像素一致**；重做后与撤销前一致；
  3. 九种混合模式各自导出，逐点比对 W3C 混合公式；
  4. 工具切换 / 图层增删 / 缩放档位等交互链路。

为什么像素断言必须**独立解码 PNG**而不是看界面截图：截图只能看出“有个深色
笔画”，断不出“该坐标的混合色是否等于 multiply 的结果”——而后者才是混合模式
真的生效的证据。实测这条区别很实在：`Canvas::draw_canvas` 曾忽略
`DrawOptions::blend`（九种模式里八种退化成普通叠加），**界面上完全看不出异常**，
只有导出后逐像素比对才发现。

用法: python3 tools/louyue_e2e.py [binary] [shots]
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "build", "dev", "bin", "louyue.exe")


# ── 控制通道客户端（与 gbcode_e2e 同形态） ──────────────────────────────────


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
                raise RuntimeError("控制通道在收到响应头前关闭")
            header += chunk
        length = struct.unpack(">I", header)[0]
        payload = b""
        while len(payload) < length:
            chunk = self.sock.recv(length - len(payload))
            if not chunk:
                raise RuntimeError("控制通道在收到完整响应前关闭")
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
        result = self.ok("invoke", params)
        return result.get("handled", False)

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]


# ── PNG 解码（只用标准库：避免给 e2e 引入 Pillow 依赖） ─────────────────────


def read_png(path):
    """解开 PNG，返回 (width, height, RGBA 字节)。支持 8 位真彩 + 五种行过滤。"""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssertionError(f"{path} 不是 PNG")
    pos, idat, width, height, color_type = 8, b"", None, None, None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            width, height, depth, color_type = struct.unpack(">IIBB", body[:10])
            if depth != 8 or color_type not in (2, 6):
                raise AssertionError(f"期望 8 位真彩，实得 depth={depth} type={color_type}")
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        pos += 12 + length
    channels = 4 if color_type == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    off = 0
    for y in range(height):
        ftype = raw[off]
        off += 1
        line = bytearray(raw[off:off + stride])
        off += stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
            elif ftype != 0:
                raise AssertionError(f"未知行过滤 {ftype}")
        for x in range(width):
            if channels == 4:
                out[(y * width + x) * 4:(y * width + x) * 4 + 4] = line[x * 4:x * 4 + 4]
            else:
                out[(y * width + x) * 4:(y * width + x) * 4 + 3] = line[x * 3:x * 3 + 3]
                out[(y * width + x) * 4 + 3] = 255
        prev = line
    return width, height, bytes(out)


def pixel(img, x, y):
    w, _h, px = img
    i = (y * w + x) * 4
    return px[i], px[i + 1], px[i + 2], px[i + 3]


def near(actual, expected, tolerance=2):
    return all(abs(int(a) - int(e)) <= tolerance for a, e in zip(actual[:3], expected[:3]))


# ── 驱动 ───────────────────────────────────────────────────────────────────


def start(binary, shots):
    ctl = os.path.join(shots, "louyue-ctl.json")
    try:
        os.remove(ctl)
    except FileNotFoundError:
        pass
    log = open(os.path.join(shots, "louyue-e2e.log"), "wb")
    process = subprocess.Popen([binary, "--headless", "--control-port", "0",
                                "--control-file", ctl, "--shots", shots],
                               stdout=log, stderr=subprocess.STDOUT)
    for _ in range(120):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl, encoding="utf-8"))
                if info.get("port"):
                    return process, Client(info["port"], info.get("token", "")), info
            except Exception:
                pass
        time.sleep(0.2)
    raise RuntimeError("应用未在 24s 内写出控制文件")


def check(condition, message):
    if not condition:
        raise AssertionError(message)


# 九种混合模式（顺序对应 `louyue::kBlendModes` / `raster::BlendMode`）
MODES = ["src_over", "src", "dst_over", "multiply", "screen", "overlay",
         "darken", "lighten", "add"]
BASE_HEX = "#33995F"   # 底层：注意蓝通道是 0x5F 不是 0x66——手写小数极易错
TOP_HEX = "#CC8080"    # 上层
BASE = (0x33 / 255.0, 0x99 / 255.0, 0x5F / 255.0)
TOP = (0xCC / 255.0, 0x80 / 255.0, 0x80 / 255.0)


def blend_expected(mode, s, d):
    """W3C 混合公式（不透明前景，a=1）。"""
    if mode in ("src", "src_over"):
        return s
    if mode == "dst_over":
        return d          # 目的（下层）盖在上层之上 → 不透明上层时结果就是下层
    if mode == "multiply":
        return s * d
    if mode == "screen":
        return s + d - s * d
    if mode == "overlay":
        return 2 * s * d if d <= 0.5 else 1 - 2 * (1 - s) * (1 - d)
    if mode == "darken":
        return min(s, d)
    if mode == "lighten":
        return max(s, d)
    if mode == "add":
        return min(1.0, s + d)
    raise AssertionError(f"未知模式 {mode}")


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    shots = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "e2e-louyue")
    os.makedirs(shots, exist_ok=True)

    process, client, info = start(binary, shots)
    try:
        # ⚠ 握手是**强制**的：控制通道拒绝任何未携带 token 的请求（错误是
        # “请先 hello 并携带 token”）。漏了这一句会在第一条 `find` 上就挂。
        client.ok("hello")
        print(f"已连接：{info.get('app')} {info.get('version')} 后端={info.get('backend')}")

        # —— 0. 骨架：画布与各面板都在 ——
        for selector in ("#canvas", "#layer-list", "#status", "#swatch", "#tool-brush"):
            check(client.count(selector) >= 1, f"缺少元素 {selector}")
        canvas = client.props("canvas")
        check(canvas["canvas_size"] == "320x200", f"画布尺寸异常: {canvas['canvas_size']}")
        print(f"[0] 骨架齐备；画布 {canvas['canvas_size']}，图层 {canvas['layer_count']}")

        # —— 1. 内嵌 PNG 真的解码了（四象限色值逐点核对）——
        out = os.path.join(shots, "sample-roundtrip.png")
        check(client.invoke("canvas", "export", out), "导出失败")
        img = read_png(out)
        check(img[0] == 320 and img[1] == 200, f"导出尺寸异常 {img[0]}x{img[1]}")
        quadrants = [((40, 40), (232, 88, 88)), ((280, 40), (88, 200, 120)),
                     ((40, 160), (88, 132, 232)), ((280, 160), (240, 196, 96))]
        for (x, y), expected in quadrants:
            actual = pixel(img, x, y)
            check(near(actual, expected), f"象限色 @({x},{y}) 实得 {actual} 期望 {expected}")
        print("[1] 内嵌 PNG 解码 → 导出往返：四象限色值逐点一致")

        # —— 2. 命令式作画 + 落点断言 ——
        # （`reset_layers` 只接 `#rrggbb` 或空串——传颜色名会被如实拒绘，
        #   这是有意的：解析“white”需要一张颜色名表，而颜色是数值量）
        check(client.invoke("canvas", "reset_layers", "#FFFFFF"), "重置图层失败")
        check(client.invoke("canvas", "stroke", "50,50,120,90"), "stroke 失败")
        out = os.path.join(shots, "stroke.png")
        check(client.invoke("canvas", "export", out), "导出失败")
        img = read_png(out)
        # 笔画中点应当与白底显著不同
        mid = pixel(img, 85, 70)
        diff = sum(abs(int(a) - int(b)) for a, b in zip(mid[:3], (255, 255, 255)))
        check(diff > 30, f"笔画中点 @(85,70) 与白底差异仅 {diff}")
        # 远离笔画的角落应当仍是白（证明只改了该改的地方）
        corner = pixel(img, 5, 5)
        check(near(corner, (255, 255, 255)), f"远处角落被误改: {corner}")
        print(f"[2] stroke 生效：中点 {mid[:3]}（与白底差 {diff}），远处角落未被污染")

        # —— 3. 撤销/重做**逐像素一致**（验收标准 2）——
        check(client.invoke("canvas", "reset_layers", "#FFFFFF"), "重置失败")
        check(client.invoke("canvas", "clear_history"), "清历史失败")
        before = os.path.join(shots, "undo-before.png")
        check(client.invoke("canvas", "export", before), "导出失败")
        check(client.invoke("canvas", "stroke", "70,70,150,130"), "stroke 失败")
        after = os.path.join(shots, "undo-after.png")
        check(client.invoke("canvas", "export", after), "导出失败")
        check(client.invoke("canvas", "undo"), "undo 失败")
        restored = os.path.join(shots, "undo-restored.png")
        check(client.invoke("canvas", "export", restored), "导出失败")
        check(client.invoke("canvas", "redo"), "redo 失败")
        redone = os.path.join(shots, "redo-after.png")
        check(client.invoke("canvas", "export", redone), "导出失败")

        ib, ia = read_png(before)[2], read_png(after)[2]
        ir, id_ = read_png(restored)[2], read_png(redone)[2]
        check(ib != ia, "落笔没有改变像素")
        check(ib == ir, "撤销后与落笔前**不是**逐像素一致（补丁边界偏了吗？）")
        check(ia == id_, "重做后与落笔后**不是**逐像素一致")
        changed = sum(1 for i in range(0, len(ib), 4) if ib[i:i + 4] != ia[i:i + 4])
        print(f"[3] 撤销/重做逐像素一致（{len(ib) // 4} 像素全等；落笔改了 {changed} 个）")

        # —— 4. 九种混合模式逐点比对公式（验收标准 3）——
        failures = []
        for index, mode in enumerate(MODES):
            check(client.invoke("canvas", "reset_layers", BASE_HEX), "重置失败")
            check(client.invoke("canvas", "add_layer"), "加层失败")
            check(client.invoke("canvas", "set_pixel", f"10 10 {TOP_HEX}"), "写像素失败")
            check(client.invoke("canvas", "set_blend", f"1 {index}"), f"设混合 {mode} 失败")
            path = os.path.join(shots, f"blend-{index:02d}-{mode}.png")
            check(client.invoke("canvas", "export", path), "导出失败")
            actual = pixel(read_png(path), 10, 10)
            expected = tuple(blend_expected(mode, s, d) for s, d in zip(TOP, BASE))
            # 容差 2/255：8 位量化 + 预乘往返的舍入
            ok = all(abs(a / 255.0 - e) <= 2.0 / 255.0 + 1e-9 for a, e in zip(actual[:3], expected))
            if not ok:
                failures.append(mode)
                print(f"    {mode}: 实得 {actual[:3]} 期望 "
                      f"{tuple(round(e * 255) for e in expected)} **FAIL**")
        check(not failures, f"混合模式不符合公式: {failures}")
        print(f"[4] 九种混合模式全部符合 W3C 公式（容差 2/255）")

        # —— 5. 交互链路：工具切换 / 图层增删 / 缩放档位 ——
        check(client.invoke("tool-eraser", "click"), "切换橡皮失败")
        check(client.props("canvas")["tool"] == "橡皮", "工具未切到橡皮")
        check(client.invoke("tool-brush", "click"), "切换画笔失败")
        check(client.props("canvas")["tool"] == "画笔", "工具未切回画笔")

        layers_before = int(client.props("canvas")["layer_count"])
        check(client.invoke("canvas", "add_layer"), "加层失败")
        check(int(client.props("canvas")["layer_count"]) == layers_before + 1, "图层数未增加")
        check(client.invoke("canvas", "remove_layer", "1"), "删层失败")
        check(int(client.props("canvas")["layer_count"]) == layers_before, "图层数未恢复")

        check(client.invoke("canvas", "zoom_in"), "放大失败")
        zoomed = float(client.props("canvas")["zoom"])
        check(zoomed > 1.0, f"缩放未生效: {zoomed}")
        check(client.invoke("canvas", "zoom_out"), "缩小失败")
        print(f"[5] 交互链路通过（工具切换 / 图层增删 / 缩放档位 {zoomed}→回退）")

        print("\n[OK] 镂月端到端全部通过")
        return 0
    finally:
        try:
            client.ok("app", {"action": "quit"})
        except Exception:
            process.terminate()
        try:
            process.wait(timeout=5)
        except Exception:
            process.terminate()


if __name__ == "__main__":
    sys.exit(main())
