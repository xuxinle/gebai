#!/usr/bin/env python3
"""把 PNG 转成**多尺寸** Windows `.ico`（无第三方依赖）。

为什么要多尺寸：Windows 会按不同场景挑不同尺寸——
  16px  任务栏 / 小图标视图
  20/24 高 DPI 下的 100%/150% 缩放
  32px  桌面 / 中等图标
  48px  大图标
  256px 超大图标视图 / 缩略图
只放一张大图，Windows 缩小后边缘会糊（尤其带透明圆角时），
而只放一张小图，超大图标视图会明显锯齿。所以一次放齐。

## 实现要点（为何不直接改写 PNG 头）

ICO 支持"内嵌 PNG"格式（Vista+），对小尺寸**不建议**——Windows 在 16/24/32
这些尺寸上对 PNG 内嵌的支持有历史坑（部分 shell 组件会拒绝渲染）。
所以这里对 <=64 的尺寸走**传统 BMP(DIB) 编码**，256 走 PNG 内嵌。
这是主流图标工具的同一策略。

DIB 编码细节（容易错、写下来）：
- 高度字段是**实际高度的两倍**（XOR 位图 + AND 掩码各占一份）；
- 像素序是**自下而上**、BGRA；
- 必须带 AND 掩码（1 位/像素，1 = 透明）——即使 32 位色有 alpha 通道，
  缺了它老组件会画成黑底。
"""
import struct
import sys
import zlib
from pathlib import Path

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False


def read_png_size(data: bytes):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("不是 PNG")
    w, h = struct.unpack(">II", data[16:24])
    bit_depth, color_type = data[24], data[25]
    return w, h, bit_depth, color_type


# —— 纯 Python 的 PNG 解码（无 Pillow 时用） ——
def _decode_png(data: bytes):
    w, h, bit_depth, color_type = read_png_size(data)
    if bit_depth != 8 or color_type not in (2, 6):
        raise ValueError(f"仅支持 8bit RGB/RGBA PNG，得到 depth={bit_depth} type={color_type}")
    channels = 3 if color_type == 2 else 4
    pos, idat = 8, b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = w * channels
    out = bytearray(h * stride)
    prev = bytearray(stride)
    src = 0
    for y in range(h):
        f = raw[src]; src += 1
        line = bytearray(raw[src:src + stride]); src += stride
        if f == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    # 归一到 RGBA
    if channels == 4:
        return w, h, bytes(out)
    rgba = bytearray(w * h * 4)
    for i in range(w * h):
        rgba[i * 4:i * 4 + 3] = out[i * 3:i * 3 + 3]
        rgba[i * 4 + 3] = 255
    return w, h, bytes(rgba)


def resize_rgba(w, h, rgba, tw, th):
    """双线性缩放（带 alpha 预乘，避免透明边缘发黑）。"""
    out = bytearray(tw * th * 4)
    sx = w / tw
    sy = h / th
    for y in range(th):
        fy = (y + 0.5) * sy - 0.5
        y0 = max(0, min(h - 1, int(fy)))
        y1 = min(h - 1, y0 + 1)
        wy = fy - y0 if fy >= 0 else 0.0
        for x in range(tw):
            fx = (x + 0.5) * sx - 0.5
            x0 = max(0, min(w - 1, int(fx)))
            x1 = min(w - 1, x0 + 1)
            wx = fx - x0 if fx >= 0 else 0.0
            acc = [0.0, 0.0, 0.0, 0.0]
            for (px, py, weight) in ((x0, y0, (1 - wx) * (1 - wy)), (x1, y0, wx * (1 - wy)),
                                     (x0, y1, (1 - wx) * wy), (x1, y1, wx * wy)):
                i = (py * w + px) * 4
                a = rgba[i + 3] / 255.0
                acc[0] += rgba[i] * a * weight
                acc[1] += rgba[i + 1] * a * weight
                acc[2] += rgba[i + 2] * a * weight
                acc[3] += rgba[i + 3] * weight
            alpha = acc[3]
            o = (y * tw + x) * 4
            if alpha > 0.5:
                out[o] = max(0, min(255, int(acc[0] / (alpha / 255.0) + 0.5)))
                out[o + 1] = max(0, min(255, int(acc[1] / (alpha / 255.0) + 0.5)))
                out[o + 2] = max(0, min(255, int(acc[2] / (alpha / 255.0) + 0.5)))
            out[o + 3] = max(0, min(255, int(alpha + 0.5)))
    return bytes(out)


def encode_dib(w, h, rgba):
    """传统 ICO 位图：BITMAPINFOHEADER + BGRA(自下而上) + AND 掩码。"""
    header = struct.pack("<IiiHHIIiiII",
                         40,        # biSize
                         w,         # biWidth
                         h * 2,     # biHeight：XOR + AND 两份
                         1,         # biPlanes
                         32,        # biBitCount
                         0,         # BI_RGB
                         0, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(h - 1, -1, -1):          # 自下而上
        row = y * w * 4
        for x in range(w):
            i = row + x * 4
            xor += bytes((rgba[i + 2], rgba[i + 1], rgba[i], rgba[i + 3]))  # BGRA
    # AND 掩码：每行按 32 位对齐
    row_bytes = ((w + 31) // 32) * 4
    and_mask = bytearray()
    for y in range(h - 1, -1, -1):
        bits = bytearray(row_bytes)
        row = y * w * 4
        for x in range(w):
            if rgba[row + x * 4 + 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        and_mask += bits
    return header + bytes(xor) + bytes(and_mask)


def build_ico(png_path: Path, ico_path: Path, sizes):
    data = png_path.read_bytes()
    src_w, src_h, rgba = _decode_png(data)
    print(f"源图 {png_path.name}: {src_w}x{src_h}")

    entries = []
    for size in sizes:
        scaled = rgba if size == src_w == src_h else resize_rgba(src_w, src_h, rgba, size, size)
        if size >= 256:
            # 256 用 PNG 内嵌（DIB 会让 ico 体积膨胀到 256KB+，而 PNG 只要几 KB）
            raw = bytearray()
            for y in range(size):
                raw.append(0)  # filter none
                raw += scaled[y * size * 4:(y + 1) * size * 4]
            def chunk(tag, payload):
                body = tag + payload
                return (struct.pack(">I", len(payload)) + body +
                        struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))
            png = b"\x89PNG\r\n\x1a\n"
            png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            png += chunk(b"IEND", b"")
            payload = png
        else:
            payload = encode_dib(size, size, scaled)
        entries.append((size, payload))
        print(f"  {size:3d}x{size:<3d} {len(payload):6d} 字节  "
              f"{'PNG' if size >= 256 else 'DIB'}")

    # ICONDIR + ICONDIRENTRY[]
    out = bytearray(struct.pack("<HHH", 0, 1, len(entries)))
    offset = 6 + 16 * len(entries)
    for size, payload in entries:
        out += struct.pack("<BBBBHHII",
                           size if size < 256 else 0,   # 0 表示 256
                           size if size < 256 else 0,
                           0, 0, 1, 32, len(payload), offset)
        offset += len(payload)
    for _, payload in entries:
        out += payload
    ico_path.write_bytes(bytes(out))
    print(f"→ {ico_path}  ({len(out)} 字节, {len(entries)} 个尺寸)")


def main():
    if len(sys.argv) < 3:
        print("用法: make_ico.py <src.png> <out.ico>")
        return 2
    png = Path(sys.argv[1])
    ico = Path(sys.argv[2])
    if not png.is_file():
        print(f"源文件不存在: {png}")
        return 2
    if HAS_PIL:
        print("（检测到 Pillow，但本脚本用内建解码以保证无依赖可复现）")
    build_ico(png, ico, [16, 24, 32, 48, 64, 128, 256])
    return 0


if __name__ == "__main__":
    sys.exit(main())
