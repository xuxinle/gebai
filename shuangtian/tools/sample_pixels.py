"""在齿轮盘面上采样，看本框架与参照的具体差异。

## 为什么不再猜

前面的分析链条：（平面法线）→（无高光）→（亮天空）→（反射系数）都修了，
主体均值只从 139.6 降到 98.7，而参照是 78.1。继续按"物理正确性"猜下去
效率太低。

## 改做直接观察

在**同一个像素位置**上把两张图的 RGB 都打出来。齿轮在两张图里的位置
基本一致（同构图），所以逐点对比能立刻看出：
- 是整体亮度差（三个通道等比）
- 还是色相不同（通道比例不同）
- 还是某些区域差异大、某些接近（说明是局部模型问题）

## 用法

    py tools/sample_pixels.py <本框架.png> <参照.png>

采样点选在齿轮盘面（大面积、低曲率）——那里最能反映"基础着色"。
"""

import struct
import sys
import zlib


def read_png(path):
    with open(path, 'rb') as handle:
        data = handle.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('不是 PNG：%s' % path)
    pos = 8
    width = height = 0
    bit_depth = color_type = 0
    idat = b''
    while pos < len(data):
        length = struct.unpack('>I', data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            width, height = struct.unpack('>II', chunk[:8])
            bit_depth, color_type = chunk[8], chunk[9]
        elif kind == b'IDAT':
            idat += chunk
        pos += 12 + length
    if bit_depth != 8 or color_type not in (2, 6):
        raise ValueError('只支持 8bit RGB/RGBA')
    channels = 4 if color_type == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(width * height * channels)
    previous = bytearray(stride)
    offset = 0
    for y in range(height):
        filter_type = raw[offset]
        offset += 1
        line = bytearray(raw[offset:offset + stride])
        offset += stride
        if filter_type == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = previous[i]
                c = previous[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predicted = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + predicted) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        previous = line
    return width, height, channels, bytes(out)


def pixel(width, channels, pixels, x, y):
    at = (y * width + x) * channels
    return pixels[at], pixels[at + 1], pixels[at + 2]


def main():
    if len(sys.argv) < 3:
        print('用法：py tools/sample_pixels.py <本框架.png> <参照.png>')
        return 2
    wa, ha, ca, pa = read_png(sys.argv[1])
    wb, hb, cb, pb = read_png(sys.argv[2])
    if (wa, ca) != (wb, cb):
        print('尺寸不同，无法逐点对比')
        return 1

    # 采样点：主齿轮盘面区域（中心偏左上，那里是齿轮实体）
    points = [
        ('主轮盘面-中心', 440, 480),
        ('主轮盘面-上', 440, 360),
        ('主轮盘面-下', 440, 600),
        ('主轮盘面-左', 340, 480),
        ('主轮盘面-右', 540, 480),
        ('轮辐位置', 470, 520),
        ('轮毂', 445, 505),
        ('齿顶-上', 420, 300),
        ('惰轮-左上', 215, 355),
        ('小轮-右上', 790, 390),
        ('小轮-右下', 745, 675),
    ]

    print('%-18s %-14s %-14s %-12s %s' % ('采样点', '本框架 RGB', '参照 RGB', '差(RGB)', '比例'))
    print('-' * 78)
    for label, x, y in points:
        if x >= wa or y >= ha:
            continue
        ra, ga, ba = pixel(wa, ca, pa, x, y)
        rb, gb, bb = pixel(wb, cb, pb, x, y)
        ratio = ''
        if rb > 0:
            ratio = 'R%.2f G%.2f B%.2f' % (ra / rb, ga / gb if gb else 0, ba / bb if bb else 0)
        print('%-18s (%3d,%3d,%3d)  (%3d,%3d,%3d)  (%+4d,%+4d,%+4d) %s' % (
            label, ra, ga, ba, rb, gb, bb, ra - rb, ga - gb, ba - bb, ratio))

    print('\n=== 判读 ===')
    print('若三个通道的**比例**一致（比如都 ×1.3），是纯亮度差（光照强度/曝光问题）')
    print('若比例不一致（尤其 B 通道偏差最大），是色相/反射模型问题')
    return 0


if __name__ == '__main__':
    sys.exit(main())
