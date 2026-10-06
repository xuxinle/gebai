#!/usr/bin/env python3
"""生成「修复前 / 修复后 / 浏览器」三方 SVG 渲染对比图。

做法：把 `stroke_to_path` 临时换回旧实现（顺时绕向 + cos(25°) 角度阈值 + 无条件圆头帽），
编一份「修复前」探针出图，再恢复当前实现出「修复后」图，与浏览器截图并排。
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path('/workspace/gebai/shuangtian')
SRC = ROOT / 'src/raster/rasterizer.cpp'
BACKUP = pathlib.Path('/tmp/rz-now.cpp')
OUT = pathlib.Path('/tmp/ab-svg3')
OUT.mkdir(exist_ok=True)

backup_text = SRC.read_text(encoding='utf-8')
BACKUP.write_text(backup_text, encoding='utf-8')

# 旧实现：绕向顺时针（抵消）、固定 cos(25°) 角度阈值、端点无条件补圆（= 旧圆头帽）
旧 = backup_text
旧 = 旧.replace("    constexpr float kMaxGapPx = 0.05f;",
              "    [[maybe_unused]] constexpr float kMaxGapPx = 0.05f;", 1)
旧 = 旧.replace("                    const StrokeStyle& style) -> Path {",
              "                    const StrokeStyle& style) -> Path {\n  (void)style;", 1)
旧 = 旧.replace("""      outline.move_to(math::Point{from.x + nx, from.y + ny});
      outline.line_to(math::Point{from.x - nx, from.y - ny});
      outline.line_to(math::Point{to.x - nx, to.y - ny});
      outline.line_to(math::Point{to.x + nx, to.y + ny});""",
"""      outline.move_to(math::Point{from.x + nx, from.y + ny});
      outline.line_to(math::Point{to.x + nx, to.y + ny});
      outline.line_to(math::Point{to.x - nx, to.y - ny});
      outline.line_to(math::Point{from.x - nx, from.y - ny});""", 1)
旧 = 旧.replace("          const float tan_half = sin_theta / (1.0f + cosine);",
              "          [[maybe_unused]] const float tan_half = sin_theta / (1.0f + cosine);", 1)
旧 = 旧.replace("          join = style.join != LineJoin::Bevel && half * tan_half > kMaxGapPx;",
              "          join = cosine < 0.9063f;  // [旧] cos(25°)", 1)
旧 = 旧.replace("""      } else if (style.cap == LineCap::Round) {""", """      } else if (true) {  // [旧] 端帽一律补圆
        (void)0; if (false) {} else
""", 1)
旧 = 旧.replace("    constexpr float kMaxGapPx = 0.05f;",
              "    [[maybe_unused]] constexpr float kMaxGapPx = 0.05f;", 1)
assert 旧 != backup_text, '旧实现替换未生效'

def build_probe():
    """**先重编框架对象，再链接探针**。

    关键：`build_one_probe.sh` 只把 `build/<profile>/obj/*.o` 链接成探针，
    **不会重编框架源码**。改了 `rasterizer.cpp` 却不重编对象，探针链接到的还是旧对象——
    "修复前 / 修复后"两份会输出**完全相同**的图（实测踩到：逐例数字一字不差，
    差点被当成"修复无效果"）。
    """
    result = subprocess.run(['./build/bin/st', 'build', 'gallery', '--profile=debug', '-j', '8'],
                            cwd=ROOT, capture_output=True, text=True)
    combined = result.stdout + result.stderr
    if result.returncode != 0:
        raise SystemExit(f'框架重编失败（exit {result.returncode}）：{combined[-800:]}')
    subprocess.run(['bash', 'tools/build_one_probe.sh', 'svg_ab_probe', 'debug'],
                   cwd=ROOT, capture_output=True)


def render(tag, dark=False):
    args = [str(ROOT / 'build/probe/svg_ab_probe'), str(OUT), '1.5']
    if dark:
        args.append('--dark')
    subprocess.run(args, cwd=ROOT, capture_output=True)
    src = OUT / ('svg-ab-dark-st.png' if dark else 'svg-ab-st.png')
    dst = OUT / f'{tag}{"-dark" if dark else ""}.png'
    dst.write_bytes(src.read_bytes())
    print(f'  出图 {dst.name}')


try:
    SRC.write_text(旧, encoding='utf-8')
    build_probe()
    render('before')
    render('before', dark=True)
finally:
    SRC.write_text(backup_text, encoding='utf-8')
    build_probe()
    render('after')
    render('after', dark=True)

print('探针已恢复为当前实现')
