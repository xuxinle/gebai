"""文档整理后的完整性核对：所有 § 引用是否仍指向存在的章节；旧值是否清零。"""
from pathlib import Path
import re, sys

root = Path('.')
problems = []

# ── ① 收集各文档里实际存在的章节号 ──
def sections(path):
    got = set()
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        m = re.match(r'^#{1,4}\s+(\d+(?:\.\d+)*)\.?\s', line)
        if m:
            got.add(m.group(1))
    return got

design_secs = sections('DESIGN.md')
conv_secs = sections('CONVENTIONS.md')
print(f"DESIGN.md 章节号: {len(design_secs)} 个")
print(f"CONVENTIONS.md 章节号: {len(conv_secs)} 个")

# ── ② 检查所有形如 "DESIGN.md §X" / "CONVENTIONS.md §X" 的引用 ──
targets = {'DESIGN.md': design_secs, 'CONVENTIONS.md': conv_secs}
files = []
for pat in ['*.md', 'docs/*.md', 'tools/*.md', 'include/**/*.hpp', 'src/**/*.cpp',
            'tests/*.cpp', 'examples/**/*.cpp', 'examples/**/*.hpp']:
    files.extend(root.glob(pat))

ref_re = re.compile(r'(DESIGN\.md|CONVENTIONS\.md)[`\s]*§\s*(\d+(?:\.\d+)*)')
checked = 0
for f in files:
    if f.name == 'README.md' and f.parent == root / 'docs':
        continue
    try:
        text = f.read_text(encoding='utf-8')
    except Exception:
        continue
    for m in ref_re.finditer(text):
        doc, sec = m.group(1), m.group(2)
        checked += 1
        if sec not in targets[doc]:
            # 允许引用"父节"（如 §8 存在但 §8.2.1 引用为 §8.2）——这里只报真正不存在的
            parents = {'.'.join(sec.split('.')[:-i]) for i in range(1, len(sec.split('.')))}
            if not (parents & targets[doc]):
                line = text[:m.start()].count('\n') + 1
                problems.append(f"{f}:{line} → {doc} §{sec} 不存在")
print(f"检查 § 引用 {checked} 处")

# ── ③ 旧值清零 ──
stale = {
    'DESIGN.md': ['102 用例 / 2018 断言', '11 条禁用特性规则', '7 处登记豁免',
                  '动画与过渡系统、文本选择与复制、更多组件（日期选择、图表）、X11/Win32 后端实测打磨',
                  '这四十六条'],
    'README.md': ['测试 312 用例', '⏳ 绘制原语与窗口呈现', '| 动画与过渡系统 | ⏳ v0.2 |',
                  '窗口实现在 v0.2'],
    'docs/cross_platform.md': ['# 10. 跨平台强制约束', '## 10.1', '## 10.6'],
    'CONVENTIONS.md': ['### 10.2 构建系统的两个'],
}
for path, needles in stale.items():
    text = Path(path).read_text(encoding='utf-8')
    for n in needles:
        if n in text:
            problems.append(f"{path}: 旧内容残留 {n!r}")

# ── ④ 新内容就位 ──
expect = {
    'DESIGN.md': ['## 目录', '### 8.2.1 这批缺陷说明了什么', '365 用例 / 10343 断言',
                  '12 条**禁用特性规则', '**365 用例'],
    'README.md': ['M1–M6 全部落地', 'docs/README.md'],
    'docs/README.md': ['霜天文档地图', '按意图索引'],
}
for path, needles in expect.items():
    text = Path(path).read_text(encoding='utf-8')
    for n in needles:
        if n not in text:
            problems.append(f"{path}: 缺少 {n!r}")

print()
if problems:
    print(f"发现 {len(problems)} 个问题：")
    for p in problems:
        print('  [X]', p)
    sys.exit(1)
print("[OK] 引用全部有效 · 旧值已清零 · 新内容就位")

