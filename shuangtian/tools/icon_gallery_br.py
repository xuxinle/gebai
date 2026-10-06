#!/usr/bin/env python3
"""图标集总览：三方对照（浏览器 / 修复前 / 修复后），**按同一顺序**排布。

两个关键点（都是踩过的坑）：
1. **顺序必须取 `IconSet::ids()` 的实际顺序**（= `Prescan::symbols` 的字典序），
   不能用 sprite 的文档序——两者不同，错位会把"图标画得对不对"完全淹没
   （实测：逐例墨量比 0.46~3.87 乱飞，实际只是拿串了图标）。
2. 单个图标也要**同尺寸、同 viewBox、同 stroke 属性**——对照才有意义。
"""
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path('/workspace/gebai/shuangtian')
SPRITE = ROOT / 'examples/gallery/assets/icons.svg'
OUT = pathlib.Path('/tmp/icons')
OUT.mkdir(exist_ok=True)
SIZES = [12, 16, 20, 24, 32, 48]
COL, ROW = 56, 40
BG = '#1E222A'
INK = '#E6EAF2'


def ids_sorted():
    """与 `IconSet::ids()` 同序：字典序（`PreScan::symbols` 是 std::map）。"""
    names = re.findall(r'<symbol id="([^"]+)"', SPRITE.read_text(encoding='utf-8'))
    return sorted(names)


def build_br(ids, path):
    parts = []
    for i, sid in enumerate(ids):
        inner = re.search(rf'<symbol id="{re.escape(sid)}"[^>]*>(.*?)</symbol>',
                          SPRITE.read_text(encoding='utf-8'), re.S).group(1)
        for r, size in enumerate(SIZES):
            left = i * COL + (COL - size) / 2
            top = r * ROW + (ROW - size) / 2
            parts.append(
                f'<svg width="{size}" height="{size}" '
                f'style="position:absolute;left:{left}px;top:{top}px" '
                f'viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" fill="none" '
                f'stroke="{INK}" stroke-width="2" stroke-linecap="round" '
                f'stroke-linejoin="round">{inner}</svg>')
    w = len(ids) * COL + 8
    h = len(SIZES) * ROW + 8
    path.write_text(
        '<!doctype html><meta charset="utf-8">'
        f'<style>html,body{{margin:0;padding:0;background:{BG}}}'
        f' body{{position:relative;width:{w}px;height:{h}px}}</style>' + ''.join(parts),
        encoding='utf-8')
    return w, h


def shot(html, png, w, h, scale=2.0):
    subprocess.run(['node', 'tools/text_ab_shot.mjs', str(html), str(png), 'icons',
                    str(scale), '0'], cwd=ROOT, capture_output=True)


def main() -> int:
    ids = ids_sorted()
    print(f'图标 {len(ids)} 个（字典序，与 IconSet::ids() 同序）：{" ".join(ids)}')
    page = ROOT / 'tools/icon_gallery_br.html'
    w, h = build_br(ids, page)
    shot(page, OUT / 'br.png', w, h)
    print('已出浏览器对照图')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
