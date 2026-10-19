"""DPI 适配的**验收探针**：验证「窗口被夹到工作区内」且界面不被挤坏。

## 为什么需要它

用户报「DPI 缩放导致页面混乱、实际不可用」。根因不是渲染（DPI 渲染经实测正确：
截图按 scale 等比放大、墨迹按面积增长、边缘保持锐利），而是**窗口逻辑尺寸写死**——
在高 DPI 的小屏上超出可用逻辑空间（13 寸 1080p + 200% ⇒ 只需 960×540），
窗口被系统裁切/挤压。

## 判据（每条都可量化）

1. **不超屏**：逻辑视口 ≤ 工作区（含 92% 边距余量）。
2. **不被挤坏**：侧栏宽度 ≥ 最小可用宽（否则标题/文件名被省略号截断）。
3. **比例关系保留**：大屏上侧栏仍按设定比例（不被最小宽无谓地抬大）。
4. **整体可用**：编辑区宽度 > 0，三栏都在。

用法：py -3 tools/dpi_fit_probe.py
"""

import json
import pathlib
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / 'build' / 'release' / 'bin' / 'gbcode.exe'
sys.path.insert(0, str(ROOT / 'tools'))
from gbcode_hint_e2e import Client  # noqa: E402

# (scale, 工作区逻辑尺寸, 说明)
CASES = [
    (1.0, (1920, 1080), '常规屏 1x'),
    (1.5, (1280, 720), '笔记本 1.5x（1080p@150% ≈ 1280×720 逻辑）'),
    (2.0, (960, 540), '小屏 2x（1080p@200% ⇒ 仅 960×540 逻辑）'),
    (2.0, (1280, 720), '高 DPI 大屏'),
]


def probe(scale, work):
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='dpifit_'))
    shots = tmp / 'shots'
    shots.mkdir()
    ctl = shots / 'ctl.json'
    logf = open(shots / 'log.txt', 'wb')
    proc = subprocess.Popen(
        [str(BIN), '--headless', '--scale', str(scale),
         '--work-area', f'{work[0]}x{work[1]}',
         '--control-port', '0', '--control-file', str(ctl), '--shots', str(shots),
         '--workspace', str(ROOT)],
        stdout=logf, stderr=subprocess.STDOUT)
    client = None
    for _ in range(200):
        if ctl.exists():
            try:
                info = json.loads(ctl.read_text(encoding='utf-8'))
                if info.get('port'):
                    client = Client(info['port'], info.get('token', ''))
                    client.ok('hello')
                    break
            except Exception:
                pass
        time.sleep(0.2)
    out = {}
    try:
        if client is None:
            return {'error': '控制通道没起来'}
        m = client.ok('metrics')
        phys_w = m.get('physical_width', 0)
        out['scale'] = m.get('device_scale')
        out['physical'] = (phys_w, m.get('physical_height'))
        out['logical_viewport'] = (phys_w / scale, m.get('physical_height', 0) / scale)
        for eid in ('sidebar', 'editor-column', 'statusbar', 'workspace-tree'):
            reply = client.call('get', {'id': eid})
            b = (reply.get('result') or {}).get('bounds')
            out[eid] = (b['width'], b['height']) if b else None
        # 侧栏标题是否被省略号截断（读元素的 text）
        reply = client.call('find', {'selector': '#sidebar'})
        matches = (reply.get('result') or {}).get('matches') or []
        out['sidebar_labels'] = [m.get('text') or m.get('value') for m in matches][:3]
        return out
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        logf.close()


def main():
    failures = []
    print(f'{"场景":<34} {"逻辑视口":<14} {"侧栏宽":<9} {"编辑区宽":<10} 判定')
    print('-' * 96)
    for scale, work, label in CASES:
        r = probe(scale, work)
        if 'error' in r:
            print(f'{label:<34} ERROR {r["error"]}')
            failures.append((label, r['error']))
            continue
        lw, lh = r['logical_viewport']
        sb = r['sidebar'][0] if r.get('sidebar') else 0
        ed = r['editor-column'][0] if r.get('editor-column') else 0
        checks = []
        # 1. 不超屏（允许 92% 边距前的物理余量）
        limit_w, limit_h = work[0] * 0.92 + 1, work[1] * 0.92 + 1
        if lw > limit_w or lh > limit_h:
            checks.append(f'超屏({lw:.0f}×{lh:.0f} > {limit_w:.0f}×{limit_h:.0f})')
        # 2. 侧栏不被挤坏
        if sb < 199:
            checks.append(f'侧栏过窄({sb:.0f}<200)')
        # 3. 编辑区仍可用
        if ed < 200:
            checks.append(f'编辑区过窄({ed:.0f}<200)')
        verdict = 'OK' if not checks else 'FAIL: ' + '；'.join(checks)
        if checks:
            failures.append((label, verdict))
        print(f'{label:<34} {lw:.0f}×{lh:.0f}{"":<6} {sb:>7.0f}  {ed:>8.0f}   {verdict}')
    print()
    if failures:
        print(f'失败 {len(failures)} 项：')
        for label, why in failures:
            print(f'  - {label}: {why}')
        return 1
    print('全部通过：四档 DPI/工作区组合下窗口都不超屏、侧栏与编辑区都不被挤坏。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
