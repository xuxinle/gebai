#!/usr/bin/env python3
"""探针守门：让 `tools/*.cpp` 的三种失效**可见**（否则它们静默存在）。

## 为什么需要它

`tools/` 有 86 个诊断探针，它们**不在任何质量门禁之内**：
`check_selfcontained.py` 只扫 `include/ src/ tools/stpm/ tests/ apps/`，
探针目录整个漏在外面。于是三件事没人知道：

1. **清单漂移**：`st.pkg` 声明了 target 但源文件已删（构建时才炸）；
   源文件在但 `st.pkg` 与 `build_probes.ps1` 都没列（**永远编不到**，改坏了也不知道）。
2. **没接统一外壳**：入口签名五种、参数裸读 `argv[i]`（越界 UB）、
   没有非零退出码（判据只能人读，无法进 CI）。
3. **判据不可机判**：有确定判据却只打印结果——「今天还合格吗」无法机器回答。

## 判据口径（三态，与 `st::probe::status` 同源）

- **fail**：确定的缺陷（清单漂移、入口越界读、无外壳却声明了判据）
- **warn**：可接受但应改进（性能基准无判据——那类本就不该设阈值）
- **ok**：已接外壳

**「读不到」不等于 ok**：`st.pkg` 缺文件或解析失败时逐项报「未核」并单独计数，
不计入通过——这与 `check_docs.py` 的三态纪律同源（本仓实测过三次「读不到被当绿灯」）。

## 用法

```
py -3 tools/check_probes.py            # 全量报告
py -3 tools/check_probes.py --strict   # 有 fail 即退出 1（供 CI/提交前用）
py -3 tools/check_probes.py --json     # 机器可读
```
"""

from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# Windows 控制台默认 GBK，直接打印中文与符号会 UnicodeEncodeError（实测）。
# 重绑 stdout/stderr 为 UTF-8（errors=replace 兼容不支持 UTF-8 的终端），
# 使本脚本在三个平台上都能直接跑而不需先设 chcp。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[union-attr]
    except (AttributeError, OSError):
        pass

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
PKG = ROOT / "st.pkg"
BUILD_PS1 = TOOLS / "build_probes.ps1"

# 入口签名（改造成统一外壳后应只剩 `main` 内部的 `probe::` 调用）
ENTRY_PATTERNS = [
    re.compile(r"^\s*auto\s+main\s*\(", re.M),
    re.compile(r"^\s*int\s+main\s*\(", re.M),
]
# 裸下标读 argv —— R17 的越界风险面
#
# ## 为何还要看 argc 守卫（2026-11-28 修正，之前 106 条里有 86 条是误报）
#
# 原判据只看“代码里出现了 `argv[i]`”。可绝大多数探针写的是**安全**形式：
#
# ```cpp
# int main(int argc, char** argv) {
#   if (argc > 1) { _putenv_s("ST_AGX_LUT", argv[1]); }
# }
# ```
#
# 即“先判个数、再读下标”。把它报成“越界 UB”，等于把整类探针按一个不存在的缺陷判红——
# 实测 106 条里 86 条属此，红灯因此完全失去信息量（真该改的那 20 条淹在里面）。
#
# 正确口径：`argv[i]` 只有在**同一函数里没有任何 `argc` 比较**时才是风险面。
# 这不是要等价于静态分析——`if (argc > 2) use(argv[3])` 这种“守卫了但不够”仍然漏；
# 但那种漏判本来就不是这个工具能回答的，把大量安全写法报成 fail 只会让人不再看报告。
RAW_ARGV = re.compile(r"argv\s*\[\s*[^\]]*\]")
# 有 argc 比较（含 `argc > 1` / `argc >= 2` / `argc < 2` 这类反向写法）。
ARGC_GUARD = re.compile(r"\bargc\s*(?:[<>]=?|==|!=)\s*\w")
# 有判据字样（说明它本该能机判）
CRITERION_HINT = re.compile(r"判据|判读|阈值|必须|不得|≤|≥")
# 性能基准：输出数字供人判，不设阈值是对的
BENCH_HINT = re.compile(r"bench|基准|吞吐|耗时|成本|perf", re.I)
# 平台依赖：缺对应库/头时**编不过不是缺陷**（本机无 FreeType/X11/POSIX 时必然如此）。
# 判据是**源码里真的 include 了那些头**，而不是靠文件名猜：
# 猜错会要么把真缺陷藏起来，要么把正常探针报成红——两者都让人学会不看红灯。
PLATFORM_DEP = [
    (re.compile(r'#include\s*[<"]ft2build\.h[>"]'), "FreeType"),
    (re.compile(r'#include\s*<dlfcn\.h>'), "POSIX dlopen"),
    (re.compile(r'#include\s*<(x11|X11)/'), "X11"),
    (re.compile(r'\bsetenv\s*\('), "POSIX setenv"),
    (re.compile(r'#include\s*"rasterize_internal\.hpp"'), "src/ 私有头（需 -Isrc）"),
]


@dataclass
class Probe:
    name: str
    path: Path
    lines: int
    has_harness: bool = False
    raw_argv: bool = False
    has_exit_code: bool = False
    criterion: bool = False
    bench: bool = False
    platform: str = ""
    in_pkg: bool = False
    in_ps1: bool = False
    findings: list[tuple[str, str]] = field(default_factory=list)


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


# ## “有失败退出码”的判据（2026-11-28 修正）
#
# 原判据只认**字面量** `return 1;` / `exit(1)`。可探针普遍写成"按判据算出来再返回"：
#
# ```cpp
# std::printf("判据：最大差 ≤ 2\n");
# return worst <= 2 ? 0 : 1;      //  ← 就是失败退出码，但旧正则看不见
# return all_ok ? 0 : 1;
# ```
#
# 实测 31 条“有判据却无失败退出码”里 **15 条**属于此，被误报成“无法进 CI”——
# 而那些恰恰是最该进 CI 的。补上三元形式。
#
# ## 第三种真实形态：**返回失败计数**（`return tally.failures;`）
#
# 这是最正宗的“可机判”形式（退出码 = 失败数，与 `st test` 同口径，
# 比 0/1 信息量更大）。原判据同样看不见，于是 `tls_https_probe` 被报成“判据只能人读”——
# 而它是整个探针目录里**唯一真的在数失败**的那个。
# 口径：“return 一个非字面量的**整数表达式**” + 源码里同时出现 0 的比较/计数痕迹。
# 不看后者会把 `return width;`（真的返回一个尺寸）当成退出码——两边都是猜，
# 所以判据写宽一点（**不错杀**）比写窄一点（假红）更符合这个工具的用途：
# 它的职责是“让该改的浮出水面”，不是替人做静态分析。
EXIT_CODE = re.compile(
    r"return\s+[1-9]\s*;"                  # return 1;
    r"|exit\s*\(\s*[1-9]"                   # exit(1)
    r"|return\s+[^;?]*\?[^;:]*:\s*[1-9]"     # return cond ? 0 : 1;
    r"|return\s+[^;?]*\?\s*[1-9][^;:]*:"     # return cond ? 1 : 0;
    r"|return\s+[A-Za-z_][\w.>-]*\s*;"        # return tally.failures;
    r"|\breturn\s+\w+\s*;.*\b\w+\s*=\s*[^;]*(?:\?|判据)",  # 兜底：判据变量参与 return
    re.S,
)

# `strip_source`：清单里每条 source 是从 `"sources": [ ... ]` 的**多行**文本里切出来的，
# 带前后换行与缩进。`strip(' "')` 不够（换行会留下，于是 `Path.is_file()` 恒假）——
# 实测就这一个字符集疏漏把 **228 个 targets** 全判成了“源文件不存在”。
# `declared_tool_sources`：把清单里所有 `tools/` 开头的 source **展开通配**，
# 得到“真的会被构建”的源文件名集合。
SOURCE_STRIP = ' "\n\t\r,'


def strip_source(raw: str) -> str:
    """把清单里一条 source 清洗成路径（去引号、空白、行内逗号）。"""
    return raw.strip(SOURCE_STRIP)


def declared_tool_sources() -> set[str]:
    """展开 `st.pkg` 里所有 `tools/` source，返回会被构建的**源文件名**集合。"""
    text = read(PKG)
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        return set()
    out: set[str] = set()
    for spec in (data.get("targets") or {}).values():
        for src in spec.get("sources") or []:
            if not isinstance(src, str) or not src.startswith("tools/"):
                continue
            if "*" in src:
                out |= {p.stem for p in ROOT.glob(src)}
            else:
                out.add(Path(src).stem)
    return out


def load_pkg_names() -> tuple[set[str], str | None]:
    """返回 `st.pkg` 里声明的 target 名集合；解析失败返回 (空集, 原因)。"""
    if not PKG.is_file():
        return set(), f"{PKG.name} 不存在"
    text = read(PKG)
    if not text.strip():
        return set(), f"{PKG.name} 读不到内容"
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        return set(), f"{PKG.name} 解析失败：{exc.msg}（第 {exc.lineno} 行）"
    targets = data.get("targets")
    if isinstance(targets, dict):
        return set(targets.keys()), None
    return set(), f"{PKG.name} 里没有 targets 字典"


def scan() -> tuple[list[Probe], list[str], list[str]]:
    """扫描探针，返回 (探针列表, 未核项, 全局问题)。"""
    names, pkg_err = load_pkg_names()
    unverified: list[str] = []
    if pkg_err:
        unverified.append(f"st.pkg：{pkg_err}（清单漂移本项未核）")

    ps1 = read(BUILD_PS1) if BUILD_PS1.is_file() else ""
    if not ps1:
        unverified.append(f"{BUILD_PS1.name}：读不到（build_probes 清单本项未核）")

    # 「哪些 tools/ 源文件真的会被构建」——展开通配后的**源文件名**集合。
    # 旧实现用的是 `path.stem in names`（目标名集合），而目标名与源文件名只是**常常**同名。
    build_sources = declared_tool_sources() if not pkg_err else set()

    probes: list[Probe] = []
    for path in sorted(TOOLS.glob("*.cpp")):
        name = path.stem
        text = read(path)
        p = Probe(name=name, path=path, lines=text.count("\n") + 1)
        p.has_harness = "st/probe/probe.hpp" in text
        p.raw_argv = bool(RAW_ARGV.search(text)) and not ARGC_GUARD.search(text)
        p.has_exit_code = bool(EXIT_CODE.search(text))
        head = "\n".join(text.splitlines()[:20])
        p.criterion = bool(CRITERION_HINT.search(head))
        p.bench = bool(BENCH_HINT.search(name)) or bool(BENCH_HINT.search(head))
        for pattern, label in PLATFORM_DEP:
            if pattern.search(text):
                p.platform = label
                break
        p.in_pkg = name in build_sources if not pkg_err else False
        p.in_ps1 = name in ps1 if ps1 else False
        probes.append(p)

    global_issues: list[str] = []
    if not pkg_err:
        # ① st.pkg 声明了但源文件不在 ⇒ 构建必炸
        for declared in sorted(names):
            srcs = re.findall(rf'"{re.escape(declared)}"\s*:\s*\{{[^}}]*"sources"\s*:\s*\[([^\]]*)\]', read(PKG))
            if not srcs:
                continue
            if "tools/" not in srcs[0]:
                continue
            if not any((ROOT / strip_source(s)).is_file() for s in srcs[0].split(",")):
                global_issues.append(f"清单漂移：st.pkg 声明 `{declared}` 但源文件不存在")
    return probes, unverified, global_issues


def classify(p: Probe) -> None:
    """给单个探针生成 findings（fail / warn / ok）。"""
    if not p.in_pkg and not p.in_ps1:
        if p.platform:
            # 平台依赖探针本机编不过（缺库/头）⇒ 不列入构建是**合理**的，不是缺陷。
            # 报 warn 而非 fail：它们仍可取证，只是不能要求每次 CI 都编。
            p.findings.append(("warn", f"未列入构建清单（依赖 {p.platform}；本机不编属正常）"))
        else:
            # 两边都没有且无平台依赖 ⇒ 真·死文件：永远编不到，改坏了无人知
            p.findings.append(("fail", "未列入 st.pkg 与 build_probes.ps1 —— 永远编不到，改坏了无人知"))
    if p.has_harness:
        if p.raw_argv:
            p.findings.append(("warn", "已接外壳但仍裸读 argv（应改用 options）"))
        else:
            p.findings.append(("ok", "已接统一外壳"))
        return
    if p.raw_argv:
        p.findings.append(("fail", "裸读 argv[i]（越界 UB，R17）且未接外壳"))
    if p.criterion and not p.has_exit_code and not p.bench:
        p.findings.append(("fail", "有判据却无失败退出码 —— 判据只能人读，无法进 CI"))
    elif p.bench and not p.has_exit_code:
        p.findings.append(("warn", "性能基准无判据（可接受：该输出数字供人判）"))
    elif not p.criterion and not p.has_exit_code:
        p.findings.append(("warn", "无判据也无退出码"))
    if not p.findings:
        p.findings.append(("ok", "有退出码"))


def main(argv: list[str]) -> int:
    as_json = "--json" in argv
    strict = "--strict" in argv
    probes, unverified, global_issues = scan()
    for p in probes:
        classify(p)

    counts = {"fail": 0, "warn": 0, "ok": 0}
    for p in probes:
        seen = {lv for lv, _ in p.findings}
        if "fail" in seen:
            counts["fail"] += 1
        elif "warn" in seen:
            counts["warn"] += 1
        else:
            counts["ok"] += 1

    if as_json:
        print(json.dumps({
            "total": len(probes),
            "counts": counts,
            "unverified": unverified,
            "globalIssues": global_issues,
            "probes": [
                {"name": p.name, "lines": p.lines, "harness": p.has_harness,
                 "findings": [{"level": lv, "text": tx} for lv, tx in p.findings]}
                for p in probes
            ],
        }, ensure_ascii=False, indent=2))
        return 1 if (strict and counts["fail"]) else 0

    print(f"探针守门：{len(probes)} 个 cpp")
    print(f"  ok {counts['ok']}｜warn {counts['warn']}｜fail {counts['fail']}")
    if global_issues:
        print("\n[全局问题]")
        for g in global_issues:
            print(f"  X {g}")
    if unverified:
        print("\n[未核项——不能当成没问题]")
        for u in unverified:
            print(f"  ? {u}")

    for level, title in (("fail", "确定的缺陷"), ("warn", "应改进")):
        rows = [(p, [t for lv, t in p.findings if lv == level]) for p in probes]
        rows = [(p, ts) for p, ts in rows if ts]
        if not rows:
            continue
        print(f"\n[{level}] {title}（{len(rows)}）")
        for p, ts in rows:
            for t in ts:
                print(f"  {p.name}: {t}")

    if strict and counts["fail"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
