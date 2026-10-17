"""文档完整性核对（v2）：§ 引用 + 旧值清零 + 新内容就位 + **代码事实核对**。

v2 新增（2026-09-30 审视后）：文档声称的数字/清单/路径与代码实测自动对照，
杜绝「DisplayList 存在」「vendor/ 目录」「组件 37 个」这类漂移再发生。
运行：python tools/check_docs.py（仓库根 cwd）

v3（2026-10-19）：③ 的“路径实存”升级为“实存 ∧ 已入仓”——只查磁盘会放过“文档引用了
只在磁盘、不在仓里”的文件（本地看着正常，`git clean`/切分支后引用悬空）。两种红分开报，
读不到 git 时明说“本项未核”而不当通过。
"""
from pathlib import Path
import re, subprocess, sys
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
# **外部仓库语境**：有些文档在讲**别人**的源码（如 `docs/SKIA_TEXT_RENDERING_STUDY.md` 引
# Skia/FreeType 的 `src/...`）。那些 `src/ports/...` 当然不在本仓——但它们**必须**保留
# 上游坐标，否则读者无法回去复查原实现（删了坐标 = 知识断了根）。
# 判据：行内/近邻出现这些词，就说明该路径属于外部仓库而不是本仓。
EXTERNAL_REPO_CTX = ('Skia', 'skia', 'FreeType', 'freetype', 'Chromium', '浏览器',
                     '上游', '外部仓库', 'HarfBuzz', 'Blink')
# 构建产物：文档指向的是“构建后才存在”的可执行目标名（源码是 `tools/<名>.cpp`）。
# 实测踩到：`tools/crash_probe` 被报“路径不存在”——而它**本就是产物名**，
# 写成源码路径反而错（读者要的是“跑这个去复现崩溃”）。所以按源码存在性放行。
BUILD_ARTIFACT_ROOTS = ('tools', 'build')
path_ref = re.compile(r'`(third_party|vendor|docs|tools|include|src|tests|examples|resources)/[A-Za-z0-9_\-./]*`')
# ⚠ **主仓根必须用绝对路径算**：`Path('.').parent` 就是 `Path('.')`——
# 原先写的 `(root.parent / ref).exists()` 与 `(root / ref).exists()` **完全等价**，
# 所谓“主仓库根回退”是死代码（实测：`docs/file-workbench-design.md` 真实存在于
# 主仓 `gebai/docs/` 下，却年年被报缺失）。用 `resolve()` 才能拿到真正的上一级。
MONOREPO_ROOT = Path('.').resolve().parent
# 检查面：原先只 6 份主文档——而**漂移最常出在专题文档里**（实测：地图本身
# 自称“六份文档”而 docs/ 已有 16 份；LOUYUE_CAIYUN.md 在实现后仍标“代码未动”）。
# 改为扫全部 .md（根级 + docs/ + tools/），口径统一。
DOC_FILES = sorted(
    [p for p in root.glob('*.md')]
    + [p for p in (root / 'docs').glob('*.md')]
    + [p for p in (root / 'tools').glob('*.md')]
)

# ── ③b 「已入仓」集合：一次 `git ls-files -z` 建集合，之后逐引用只做集合查找 ──
#
# ## 为什么“路径实存”不够（本仓三次实测，根因完全相同）
#
# 只查 `Path.exists()` 的 ③ 有个洞：文档引用了**只在磁盘、不在仓里**的文件——
#   ① pty 四工具（第 27 息）：`docs/BACKLOG.md` 与用例注释引用 `tools/pty_timeline.py` /
#      `tools/pty_form_reverse_verify.py`，而它们还是 `??`；
#   ② wait_probe.sh（第 58 息）：`pgrep-lock.ts` 头注释与 `docs/PITFALLS.md` 都引用了那批
#      实测数据，而脚本在 `/tmp`；
#   ③ lease 三脚本（第 61 息）：`DESIGN.md:1543` 引用 `lease_accept_live_1017.ts`，它从未入仓。
# 三次的形态一致：**本地看着一切正常**。磁盘有、仓里没有 ⇒ 一旦有人 `git clean`／切分支／
# 重启，引用就变成悬空，而校验器全程报绿。所以判据要从「磁盘存在」升级为「磁盘存在 ∧ 已入仓」。
#
# ## 两种红必须分开报（后果完全不同）
#   · `路径不存在`：真的写错了／文件被删了——读者当场就找不到;
#   · `路径存在但未入仓`：**三次的真形态**——现在读得到，清理/切换后就消失。
# 合成一句报错会让人按“写错路径”去查，而实际要做的是 `git add` 或删掉引用。
#
# ## 三态纪律（与 `commit-scope.ts` 的 collect review 同型）
# 读不到 git（不在仓内／没装 git／命令失败）**≠ 没问题**。此时逐项明说「本项未核」并在
# 汇总行单独计数——**不计入通过**。绝不让“读不到”静默变成绿灯（那等于在最需要警报的
# 环境里安静）。注意：`git ls-files --cached` 只列**文件**，从不列目录，所以目录引用必须
# 按「有已跟踪文件以它开头」判（否则 `src/`、`examples/gbcode` 这类目录引用会被误报
# “未入仓”——本层实测第一版就是错在这里，52 处假红）。
def _git_out(args, cwd=None):
    """跑一条 git 命令：成功返回 (stdout_bytes, '')，失败返回 (None, 原因)。"""
    try:
        r = subprocess.run(['git'] + list(args), capture_output=True, cwd=cwd)
    except (OSError, subprocess.SubprocessError) as e:
        return None, f'git 不可执行（{type(e).__name__}）'
    if r.returncode != 0:
        first = r.stderr.decode('utf-8', 'replace').strip().splitlines()
        return None, (f"git {' '.join(args)} 返回 {r.returncode}"
                      + (f"（{first[0][:80]}）" if first else ''))
    return r.stdout, ''

GIT_ROOT = None          # 仓库根（绝对、已 resolve），用于把磁盘路径换算成 ls-files 的键
TRACKED = set()          # git ls-files --cached 的文件键集合（相对仓库根）
TRACKED_DIRS = set()     # 由上面派生：所有“含已跟踪文件”的目录前缀
GIT_UNAVAILABLE = ''     # 非空 ⇒ 入仓性**本项未核**（其原因）

_raw, _why = _git_out(['rev-parse', '--show-toplevel'])
if _raw is None:
    GIT_UNAVAILABLE = _why
else:
    GIT_ROOT = Path(_raw.decode('utf-8', 'replace').strip()).resolve()
    # ⚠ 必顶 `cwd=GIT_ROOT`：`git ls-files` 在子目录里跑时**只列 cwd 下的文件**且输出
    # **相对 cwd** 的路径。而下面 `repo_key()` 给的是相对**仓库根**的键（`shuangtian/tools/x.py`）
    # ——两者直接拼一起就是“键对不上”，实测第一版因此把 687 处已入仓的引用全部报成未入仓。
    _raw, _why = _git_out(['ls-files', '-z', '--cached'], cwd=GIT_ROOT)
    if _raw is None:
        GIT_UNAVAILABLE = _why
    else:
        TRACKED = {s for s in _raw.decode('utf-8', 'replace').split('\0') if s}
        for _t in TRACKED:
            _parts = _t.split('/')
            for _i in range(1, len(_parts)):
                TRACKED_DIRS.add('/'.join(_parts[:_i]))
        if not TRACKED:
            GIT_UNAVAILABLE = '仓库里没有任何已跟踪文件（空仓？）'

def repo_key(disk):
    """磁盘路径 → `git ls-files` 的键（相对仓库根）；换算不出（仓外）返回 None ⇒ 本项未核。"""
    if GIT_ROOT is None:
        return None
    try:
        return disk.resolve().relative_to(GIT_ROOT).as_posix()
    except (ValueError, OSError):
        return None

# ③ 的两层判定的计数（必须能对账：total = 各去向之和）
ref_total = 0        # 正则抽到的路径引用总数
ref_glob = 0         # glob/占位符/上级相对：不核
ref_removal = 0      # 移除/历史语境放行
ref_external = 0     # 外部仓库语境放行
ref_tracked = 0      # 实存 ∧ 已入仓
ref_untracked = 0    # 实存 ∧ 未入仓  ← 三次事故的真形态
ref_missing = 0      # 磁盘上不存在
ref_unverified = []  # 实存但读不到 git ⇒ 本项未核

for p in DOC_FILES:
    text = p.read_text(encoding='utf-8')
    doc_lines = text.splitlines()
    for m in path_ref.finditer(text):
        ref = m.group(0).strip('`')
        ref_total += 1
        if '*' in ref or '<' in ref or '{' in ref:
            ref_glob += 1
            continue  # glob/占位符
        if '..' in ref:
            ref_glob += 1
            continue  # 显式相对上级（写法本身已表明“不在本工程内”）
        line_no = text[:m.start()].count('\n') + 1
        ctx = ' '.join(doc_lines[max(0, line_no - 3):min(len(doc_lines), line_no + 2)])
        if any(k in ctx for k in REMOVAL_CTX):
            ref_removal += 1
            continue  # 移除记录语境（前后 2 行）：故意引用已不存在的历史路径
        if any(k in ctx for k in EXTERNAL_REPO_CTX):
            ref_external += 1
            continue  # 外部仓库语境：保留上游坐标（不是本仓路径）

        # ── 第一层：磁盘实存（原有判据，语义不变）──
        disk = None
        if (root / ref).exists():
            disk = root / ref
        elif (MONOREPO_ROOT / ref).exists():
            disk = MONOREPO_ROOT / ref  # 主仓库根（shuangtian/ 的上一级）：docs/、resources/ 等主仓文件
        elif ref.startswith(BUILD_ARTIFACT_ROOTS):
            # 构建产物：`tools/<名>` 这类指“编出来的可执行目标”，源码存在即算成立。
            leaf = ref.rsplit('/', 1)[-1]
            for d in ('tools',):
                cand = root / d / f'{leaf}.cpp'
                if cand.exists():
                    disk = cand
                    break
        if disk is None:
            ref_missing += 1
            problems.append(f"{p}:{line_no} 引用的路径不存在: {ref}")
            continue

        # ── 第二层：已入仓（本次新增；磁盘有 ≠ 仓里有）──
        key = repo_key(disk)
        if key is None or GIT_UNAVAILABLE:
            ref_unverified.append(
                f"{p}:{line_no} `{ref}` — 本项未核（{GIT_UNAVAILABLE or '路径不在 git 仓库根下' }）")
            continue
        if key in TRACKED or key.rstrip('/') in TRACKED_DIRS:
            ref_tracked += 1
            continue
        ref_untracked += 1
        problems.append(
            f"{p}:{line_no} 引用的 `{ref}` 存在于磁盘但**未被 git 跟踪**"
            f"（`git clean`/切换分支后会丢）——请入仓或在文档里去掉该引用")

print(f"引用入仓核对: 抽到路径引用 {ref_total} 处 → 实存∧入仓 {ref_tracked} 处 · 未入仓 {ref_untracked} 处 · "
      f"不存在 {ref_missing} 处 · 移除/外部语境放行 {ref_removal + ref_external} 处 · "
      f"glob/上级相对不核 {ref_glob} 处")
if GIT_UNAVAILABLE:
    print(f"  入仓性**本项未核** {len(ref_unverified)} 处（读不到 git：{GIT_UNAVAILABLE}）——"
          f"它们**不计入**上面的“实存∧入仓")
    for u in ref_unverified[:10]:
        print('  [~]', u)
    if len(ref_unverified) > 10:
        print(f'  [~] …还有 {len(ref_unverified) - 10} 处')
else:
    print(f"  入仓性已核 {ref_tracked + ref_untracked}/{ref_tracked + ref_untracked} 处（git ls-files 共 "
          f"{len(TRACKED)} 个已跟踪文件 / {len(TRACKED_DIRS)} 个派生目录）")

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

# ── ④b 组件**计数**核对：文档里「N 个内置组件」的 N 必须等于注册表实际条目数 ──
#
# 为何补这一条（2026-10-14）：④ 只核对“清单里的名字存在”，管不到散在正文里的**计数**。
# 于是 `ST_COMPONENT_LIST` 从 32 长到 48 的过程中，三处「32 个内置组件」的表述
# （`dsl.hpp` 两处 + `DESIGN.md` 一处）全部滞后——而这份工具**自称**核对「组件清单」。
# 名字对得上、数字对不上，是两份口径分家的典型形态：读者按“32”估算组件库规模，
# 而实际有 48。这一条把它钉成可自动判定的断言。
dsl_cpp = root / 'src' / 'ui' / 'dsl.cpp'
if dsl_cpp.exists():
    dsl_text = dsl_cpp.read_text(encoding='utf-8')
    block = re.search(r'#define ST_COMPONENT_LIST\(X\)(.*?)\n\n', dsl_text, re.S)
    if block:
        actual_count = len(re.findall(r'^\s*X\(', block.group(1), re.M))
        count_sources = [root / 'DESIGN.md', root / 'include' / 'st' / 'ui' / 'dsl.hpp']
        marker = '个内置组件'
        reported = 0
        for src in count_sources:
            if not src.exists():
                continue
            for line_no, line in enumerate(src.read_text(encoding='utf-8').splitlines(), 1):
                idx = line.find(marker)
                while idx != -1:
                    # 往 marker 前面回扫：**先跳过空白**（文本是「48 个内置组件」——
                    # 数字与「个」之间有空格），再扫连续数字。
                    # 不用正则：这段脚本的 `\d` 曾因转义层数写成字面量，
                    # 静默把 48 读成 4（本次补丁的实测记录）。
                    end = idx
                    while end > 0 and line[end - 1].isspace():
                        end -= 1
                    begin = end
                    while begin > 0 and line[begin - 1].isdigit():
                        begin -= 1
                    if begin < end:
                        num = line[begin:end]
                        reported += 1
                        if int(num) != actual_count:
                            problems.append(
                                f"{src.relative_to(root)}:{line_no} 组件计数漂移: "
                                f"写「{num} 个内置组件」，实际 {actual_count} 个")
                    idx = line.find(marker, idx + len(marker))
        print(f"组件计数核对: 注册表 {actual_count} 个，文档引用 {reported} 处已比对")

# ── ⑤ 用例数/文件数核对：DESIGN §8.5 的数字必须与代码实测一致（终值，不允许滞后或虚报） ──
st_test_count = 0
st_assert_hint = 0
for f in (root / 'tests').glob('*_test.cpp'):
    src = f.read_text(encoding='utf-8', errors='ignore')
    # 两种注册宏都算：`ST_TEST` 与 `ST_TEST_WITH_TIMEOUT`（后者给集成级用例
    # 指定软超时——它也是“一个用例”，漏数会让文档里的用例数对不上）。
    st_test_count += len(re.findall(r'\bST_TEST\s*\(', src))
    st_test_count += len(re.findall(r'\bST_TEST_WITH_TIMEOUT\s*\(', src))
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

# ── ⑤b BACKLOG 结构：待做项必须全在 P0/P1/P2 章内（2026-10-09 整理后加的防复发）──
#
# 为何要查：待做项散在历史章里"肉眼扫不到等于不存在"。实测（2026-10-09）：
# **39 条**未完成项散落在 5 个章下，其中 4 条落在标着「已完成」的章里、4 条落在
# 「P0（当前无）」章里——而文件头部却声称"本文件是唯一权威清单"。
# 另一类静默丢失：用 `### [ ]` 标题形式写待做（不走 `- [ ]` 列表），既不进统计也
# 不被工具看见（实测 2 条）。
BACKLOG_PRIORITY_SECTIONS = ('## P0', '## P1', '## P2')
bl_path = root / 'docs' / 'BACKLOG.md'
if bl_path.exists():
    bl_lines = bl_path.read_text(encoding='utf-8').splitlines()
    section = ''
    strays = []
    for ln, line in enumerate(bl_lines, 1):
        if re.match(r'^## ', line):
            section = line.strip()
        if re.match(r'^\s*-\s*\[ \]', line):
            if not section.startswith(BACKLOG_PRIORITY_SECTIONS):
                strays.append(f"L{ln} 在「{section[:40]}」下")
        # `### [ ]` 形式的待做：同样要报（格式不一致 = 不进统计）
        if re.match(r'^###\s*\[ \]', line):
            strays.append(f"L{ln} 用了 `### [ ]` 标题形式（请改为 `- [ ]` 列表项）")
    if strays:
        problems.append("docs/BACKLOG.md 待做项不在 P0/P1/P2 章内（共 "
                        f"{len(strays)} 处）：" + "; ".join(strays[:6]))
    print(f"BACKLOG 结构核对：待做项均在 P0/P1/P2 章内")

# ── ⑤c PITFALLS.md 条目格式：每条必须带「表头」与「出处」（防退化成流水账）──
#
# 为何要查：这份表是**按场景检索的索引**，价值全在“每条能定位到详情”。
# 若允许无出处的条目混进来，它会变成又一个“讲了但没法查”的大文件——
# 而“堆在大文件里按时间归档”正是它要解决的问题（见 DOC_USABILITY_FINDINGS.md）。
# 格式约定：每个条目是一行表格行，最后两列必须分别有「一句话」与「出处」且出处非空。
pf = root / 'docs' / 'PITFALLS.md'
if pf.exists():
    pf_bad = []
    for ln, line in enumerate(pf.read_text(encoding='utf-8').splitlines(), 1):
        if not line.startswith('|'):
            continue
        # ⚠ 表格行的列解析有两个坑（都踩过，均由植入验证暴露）：
        #   ① `line.strip('|')` 会把**空的出处格也剥掉**：`| a | b | |` → 只剩 2 格；
        #   ② 先 `split('|')` 再 `[1:-1]` 同样会切掉尾部空串。
        # 正确：**先 rstrip/lstrip 掉行首行尾的边界管**，再切分——空列得以保留。
        body = line.rstrip('|').lstrip('|')
        cells = [c.strip() for c in body.split('|')]
        if all(set(c) <= set('-: ') for c in cells):
            continue  # 表头分隔行（`|---|---|---|`）
        if len(cells) < 3:
            pf_bad.append(f"L{ln} 列数不足（应为三列：坑 / 一句话 / 出处）")
            continue
        if cells[0] in ('坑', '现象'):
            continue  # 表头行（只认第一列的字面标题）
        # ⚠ 不要去判 `cells[1] == '一句话'`：植入验证时用“一句话”作正文内容，
        # 结果该违规行被当成表头**静默跳过**（假护栏）。表头只靠首列识别。
        if not cells[-1]:
            pf_bad.append(f"L{ln} 条目缺「出处」")
    if pf_bad:
        problems.append(f"docs/PITFALLS.md 条目格式不合规（共 {len(pf_bad)} 处）："
                        + "; ".join(pf_bad[:6]))
    # 每条出处里写的文档必须真实存在（防写一个幻想出来的出处）
    pf_text = pf.read_text(encoding='utf-8')
    for m in re.finditer(r'`(CONVENTIONS\.md|DESIGN\.md|BACKLOG\.md|perceptual_changes\.md|'
                         r'PAINT_DIAGNOSIS\.md|declarative\.md|DOC_USABILITY_FINDINGS\.md)`', pf_text):
        name = m.group(1)
        if not (root / name).exists() and not (root / 'docs' / name).exists():
            problems.append(f"docs/PITFALLS.md 出处指向不存在的文件: {name}")
    print(f"PITFALLS 格式核对：条目均带出处")

# ── ⑥ 新内容就位 ──
expect = {
    'DESIGN.md': ['## 目录', '### 8.2.1 这批缺陷说明了什么', '16 条**禁用特性规则'],
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
# ── 三态收尾：**红 / 绿 / 未核**必须分开，且“未核”不得落在绿里 ──
#
# 为何要单独一段（本层实测踩到）：第一版只把“本项未核”**打印**出来，没进 `problems`——
# 于是在一个没有 .git 的目录里跑（验证器自身可移植的场景），它会以 **exit 0 + [OK]** 收尾，
# 而 1 处引用根本没被核过。那正是“在最需要警报的环境里安静”（与 `commit-scope.ts` 的
# collect review 同型）：读者看到绿灯，以为入仓性核过了。
# 正确的三态：有错 ⇒ 红（exit 1）；无可核但读不到 git ⇒ **未核**（exit 2，不当通过）；
# 全部核过且无异常 ⇒ 绿（exit 0，且明说“已核 N/N”）。
if problems:
    print(f"发现 {len(problems)} 个问题：")
    for p in problems:
        print('  [X]', p)
    if ref_unverified:
        print(f"另有 {len(ref_unverified)} 处入仓性**本项未核**（{GIT_UNAVAILABLE or '路径不在 git 仓库根下'}）")
    sys.exit(1)
if ref_unverified:
    print(f"[未核] 其余检查均通过，但入仓性有 {len(ref_unverified)} 处**本项未核**："
          f"{GIT_UNAVAILABLE or '路径不在 git 仓库根下'}")
    print("  按三态纪律：未核 ≠ 通过。入仓性校验请在有 git 的仓库里重跑。")
    sys.exit(2)
print("[OK] § 引用有效 · 旧值清零 · 路径实存且已入仓 · 组件/用例数与代码一致 · BACKLOG 结构合规 · "
      "PITFALLS 格式合规 · 新内容就位")
