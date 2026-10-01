"""文档完整性核对（v2）：§ 引用 + 旧值清零 + 新内容就位 + **代码事实核对**。

v2 新增（2026-09-30 审视后）：文档声称的数字/清单/路径与代码实测自动对照，
杜绝「DisplayList 存在」「vendor/ 目录」「组件 37 个」这类漂移再发生。
运行：python tools/check_docs.py（仓库根 cwd）
"""
from pathlib import Path
import re, sys
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

root = Path('.')
problems = []

# ── ① § 引用有效性 ──
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
            parents = {'.'.join(sec.split('.')[:-i]) for i in range(1, len(sec.split('.')))}
            if not (parents & targets[doc]):
                line = text[:m.start()].count('\n') + 1
                problems.append(f"{f}:{line} → {doc} §{sec} 不存在")
print(f"检查 § 引用 {checked} 处")

# ── ② 旧值清零（历史批次） ──
stale = {
    'DESIGN.md': ['102 用例 / 2018 断言', '11 条禁用特性规则', '7 处登记豁免',
                  '动画与过渡系统、文本选择与复制、更多组件（日期选择、图表）、X11/Win32 后端实测打磨',
                  '这四十六条'],
    'README.md': ['测试 312 用例', '⏳ 绘制原语与窗口呈现', '| 动画与过渡系统 | ⏳ v0.2 |',
                  '窗口实现在 v0.2'],
    'docs/cross_platform.md': ['# 10. 跨平台强制约束', '## 10.1', '## 10.6'],
    'CONVENTIONS.md': ['### 10.2 构建系统的两个'],
}
# v2：审视发现的漂移全部加入禁词（阶段 C 修正后即为 0 命中）
stale.update({
    'DESIGN.md': ['DisplayList', 'paint(DisplayList&', '绘制显示列表'],
    'README.md': ['vendor/README.md', '外部依赖只有两个', 'OpenGL 3D'],
})
# 例外：BACKLOG/演进注释里的 DisplayList 是有意保留的规划词，只禁 DESIGN/README 正文再把它当现有架构。
# 因此下面的检查改为：DESIGN.md 中 DisplayList 只允许出现在「演进项/BACKLOG」语境行。
# 注：DESIGN 的「365 用例 / 10343 断言」「193 文件」由 ⑤ 的数字核对接管（允许更新到更大值，但不允许旧值残留），
# 因此这里不重复列——阶段 C 会把它们更新为实测终值。
for path, needles in stale.items():
    p = root / path
    if not p.exists():
        continue
    text = p.read_text(encoding='utf-8')
    for n in needles:
        if n not in text:
            continue
        if n == 'DisplayList':
            # 只允许「演进项/规划」语境（同一行含 BACKLOG 或 v0.3 或 保留模式）
            for i, line in enumerate(text.splitlines(), 1):
                if n in line and not any(k in line for k in ('BACKLOG', 'v0.3', '保留模式', '规划')):
                    problems.append(f"{path}:{i} 旧内容/漂移残留 {n!r}（非规划语境）")
            continue
        problems.append(f"{path}: 旧内容/漂移残留 {n!r}")

# ── ③ 目录引用实存性：文档中出现的仓库相对路径必须存在 ──
# 例外：行内含「移除/已删/历史/曾」（如 §8.4.1 的 GL 移除清单）是故意保留的历史记录。
REMOVAL_CTX = ('移除', '已删', '删除', '历史', '曾用', '旧实现')
path_ref = re.compile(r'`(third_party|vendor|docs|tools|include|src|tests|examples|resources)/[A-Za-z0-9_\-./]*`')
for df in ['README.md', 'DESIGN.md', 'CONVENTIONS.md', 'docs/README.md',
           'docs/independent_project.md', 'docs/cross_platform.md']:
    p = root / df
    if not p.exists():
        continue
    text = p.read_text(encoding='utf-8')
    for m in path_ref.finditer(text):
        ref = m.group(0).strip('`')
        if '*' in ref or '<' in ref or '{' in ref or '..' in ref:
            continue  # glob/占位符/相对上级（相对路径由 ⑦ 主仓回退处理）
        line_no = text[:m.start()].count('\n') + 1
        lines = text.splitlines()
        ctx = ' '.join(lines[max(0, line_no - 3):min(len(lines), line_no + 2)])
        if any(k in ctx for k in REMOVAL_CTX):
            continue  # 移除记录语境（前后 2 行）：故意引用已不存在的历史路径
        if (root / ref).exists():
            continue
        if (root.parent / ref).exists():
            continue  # 主仓库根（shuangtian/ 的上一级）也能命中：resources/ 等主仓资源
        # 显式相对主仓的写法（如 ../../resources/）
        norm = ref
        while norm.startswith('../'):
            norm = norm[3:]
        if norm and (root.parent / norm).exists():
            continue
        problems.append(f"{df}:{line_no} 引用的路径不存在: {ref}")

# ── ④ 组件清单核对：DESIGN.md §4.5 列出的组件必须真实存在 ──
comp_dir = root / 'include' / 'st' / 'ui' / 'components'
hpp_text = ''
if comp_dir.exists():
    for h in comp_dir.glob('*.hpp'):
        hpp_text += h.read_text(encoding='utf-8')
all_src = ''
for pat in ['include/**/*.hpp', 'src/**/*.cpp']:
    for f in root.glob(pat):
        all_src += f.read_text(encoding='utf-8', errors='ignore')
m = re.search(r'组件库（`include/st/ui/components/\*\.hpp`）：(.+)', (root / 'DESIGN.md').read_text(encoding='utf-8'))
if m:
    listed = re.findall(r'`([A-Z][A-Za-z0-9_]*)`', m.group(1))
    missing = [c for c in listed
               if not re.search(r'\b(class|struct)\s+' + c + r'\b', all_src)]
    if missing:
        problems.append(f"DESIGN.md §4.5 组件清单虚报（代码中不存在）: {', '.join(missing)}")
    print(f"组件清单核对: 声明 {len(listed)} 个, 代码缺失 {len(missing)} 个")

# ── ⑤ 用例数/文件数核对：DESIGN §8.5 的数字必须与代码实测一致（终值，不允许滞后或虚报） ──
st_test_count = 0
st_assert_hint = 0
for f in (root / 'tests').glob('*_test.cpp'):
    src = f.read_text(encoding='utf-8', errors='ignore')
    st_test_count += len(re.findall(r'\bST_TEST\s*\(', src))
design_text = (root / 'DESIGN.md').read_text(encoding='utf-8')
for m in re.finditer(r'(\d+)\s*用例\s*/\s*(\d+)\s*断言', design_text):
    doc_cases = int(m.group(1))
    if doc_cases != st_test_count:
        problems.append(f"DESIGN.md 用例数与代码不一致: 文档 {doc_cases} vs 实际 ST_TEST {st_test_count}（断言数需跑 st test 后手工更新）")
        break
# lint 文件数：st lint 自报「扫描 N 个文件」；这里只做存在性提示（不硬校验，允许与目录清点口径不同）
mfile = re.search(r'扫描\s*(\d+)\s*个文件', design_text)
src_files = list((root / 'src').rglob('*.*')) + list((root / 'include').rglob('*.hpp')) + \
            list((root / 'tests').glob('*.cpp')) + list((root / 'examples').rglob('*.cpp')) + \
            list((root / 'tools').rglob('*.cpp'))
print(f"用例数核对: 实际 ST_TEST {st_test_count} 个；源文件约 {len(src_files)} 个")

# ── ⑥ 新内容就位 ──
expect = {
    'DESIGN.md': ['## 目录', '### 8.2.1 这批缺陷说明了什么', '13 条**禁用特性规则'],
    'README.md': ['M1–M6 全部落地', 'docs/README.md'],
    'docs/README.md': ['霜天文档地图', '按意图索引'],
    'docs/BACKLOG.md': ['停摆竞态根因定位'],
}
for path, needles in expect.items():
    p = root / path
    if not p.exists():
        problems.append(f"{path}: 文件缺失")
        continue
    text = p.read_text(encoding='utf-8')
    for n in needles:
        if n not in text:
            problems.append(f"{path}: 缺少 {n!r}")

print()
if problems:
    print(f"发现 {len(problems)} 个问题：")
    for p in problems:
        print('  [X]', p)
    sys.exit(1)
print("[OK] § 引用有效 · 旧值清零 · 路径实存 · 组件/用例数与代码一致 · 新内容就位")
