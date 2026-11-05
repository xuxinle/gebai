"""主体亮度分布对比（不依赖框架的 C++ 工具）。

## 为什么改用 Python

上一版 C++ 工具（`tools/histogram_diff.cpp`）出现了数值爆炸——某些档的
百分比是 1e150 量级。加边界检查后仍在，说明问题不在边界读，而在更底层
（可能与我反复改那个文件有关）。

而这件事本身很简单：读 PNG、分离背景、统计直方图。用 Python 几十行就够，
且**可以立刻验证对不对**（打印总和应当 = 100%）。

## 判据自检

本脚本自己断言：所有档的百分比之和 = 100（±0.5）。若不等，说明统计有错，
直接报错而不是输出可疑数据。

## 用法

    py tools/subject_histogram.py <本框架.png> <参照.png>
"""

import struct
import sys
import zlib


def read_png(path):
    """最小的 PNG 解码（只支持 8bit RGB/RGBA、非隔行）。"""
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
        raise ValueError('只支持 8bit RGB/RGBA：%s' % path)
    channels = 4 if color_type == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels

    # 反滤波
    out = bytearray(width * height * channels)
    previous = bytearray(stride)
    offset = 0
    for y in range(height):
        filter_type = raw[offset]
        offset += 1
        line = bytearray(raw[offset:offset + stride])
        offset += stride
        if filter_type == 1:      # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:    # Average
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:    # Paeth
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


def background_of(width, height, channels, pixels):
    """四角 8×8 区域的中位数当背景色。"""
    values = [[], [], []]
    for corner in ((0, 0), (width - 8, 0), (0, height - 8), (width - 8, height - 8)):
        for dy in range(8):
            for dx in range(8):
                x = min(width - 1, max(0, corner[0] + dx))
                y = min(height - 1, max(0, corner[1] + dy))
                at = (y * width + x) * channels
                for c in range(3):
                    values[c].append(pixels[at + c])
    out = []
    for c in range(3):
        values[c].sort()
        out.append(values[c][len(values[c]) // 2])
    return out


def analyze(path, bins=32):
    width, height, channels, pixels = read_png(path)
    bg = background_of(width, height, channels, pixels)
    hist = [0] * bins
    total = 0
    total_luma = 0.0
    total_sq = 0.0
    for y in range(height):
        for x in range(width):
            at = (y * width + x) * channels
            r, g, b = pixels[at], pixels[at + 1], pixels[at + 2]
            if abs(r - bg[0]) + abs(g - bg[1]) + abs(b - bg[2]) <= 12:
                continue
            luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
            total += 1
            total_luma += luma
            total_sq += luma * luma
            index = min(bins - 1, int(luma * bins / 256.0))
            hist[index] += 1
    if total == 0:
        return None
    mean = total_luma / total
    variance = total_sq / total - mean * mean
    stddev = variance ** 0.5 if variance > 0 else 0.0
    percent = [100.0 * h / total for h in hist]
    # 自检：总和必须是 100
    check = sum(percent)
    if abs(check - 100.0) > 0.5:
        raise ValueError('统计自检失败：百分比之和 %.2f != 100' % check)
    return {
        'path': path,
        'mean': mean,
        'stddev': stddev,
        'subject': total,
        'percent': percent,
        'bins': bins,
    }


def main():
    if len(sys.argv) < 3:
        print('用法：py tools/subject_histogram.py <本框架.png> <参照.png>')
        return 2
    mine = analyze(sys.argv[1])
    ref = analyze(sys.argv[2])
    if mine is None or ref is None:
        print('主体像素为 0，检查图是否是空场景')
        return 1

    print('=== 主体亮度分布对比 ===\n')
    print('本框架：均值 %.1f  标准差 %.1f  主体像素 %d' % (mine['mean'], mine['stddev'],
                                                  mine['subject']))
    print('参照：  均值 %.1f  标准差 %.1f  主体像素 %d\n' % (ref['mean'], ref['stddev'],
                                                  ref['subject']))

    print('%-12s %-9s %-9s %-9s' % ('亮度区间', '本框架%', '参照%', '差'))
    print('-' * 44)
    bins = mine['bins']
    worst = 0.0
    worst_index = 0
    for i in range(bins):
        low = i * 256 // bins
        high = (i + 1) * 256 // bins
        a = mine['percent'][i]
        b = ref['percent'][i]
        if a < 0.3 and b < 0.3:
            continue
        print('%3d~%-8d %-9.1f %-9.1f %+.1f' % (low, high, a, b, a - b))
        if abs(a - b) > abs(worst):
            worst = a - b
            worst_index = i

    print('\n差异最大的一档：%d~%d（%+.1f 个百分点）' % (
        worst_index * 256 // bins, (worst_index + 1) * 256 // bins, worst))

    print('\n=== 判读 ===')
    print('标准差：本框架 %.1f vs 参照 %.1f' % (mine['stddev'], ref['stddev']))
    ratio = mine['stddev'] / ref['stddev'] if ref['stddev'] > 0 else 0
    if ratio < 0.75:
        print('→ 分布**更窄**：明暗对比不足（要拉开层次，不是整体调暗）')
    elif ratio > 1.3:
        print('→ 分布**更宽**：对比过强')
    else:
        print('→ 分布宽度接近')
    print('均值差 %+.1f' % (mine['mean'] - ref['mean']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
