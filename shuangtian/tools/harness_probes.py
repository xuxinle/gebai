#!/usr/bin/env python3
"""把探针的**外壳**接到 `st::probe`，测量逻辑一行不动。

## 它改什么 / 不改什么

改（机械、可逐条核对）：
1. 入口签名 → `auto main(int argc, char** argv) -> int` + `options::from`
2. `--help` / `--json` 由外壳接管（取代手写用法段落）
3. 「条件不具备」的早退（字体缺失等）→ `st::probe::skip(原因)`，退出码 77
   **不是** `return 1`：没测成与测过不合格是两件事，混同会让 CI 把「本机没字体」
   当成「有问题」，久了就没人看红灯
4. 末尾 `return 0` → `st::probe::main_of(spec, opts, verdict)`

**不改**：任何测量语句、阈值、打印内容。判据的「合格线」需要人读注释决定，
脚本不猜——猜错会静默改变量规含义。

## 用法

```
py -3 tools/harness_probes.py --dry-run        # 只报会改什么
py -3 tools/harness_probes.py --apply --only a_probe,b_probe
py -3 tools/harness_probes.py --apply          # 全部可机械改造的
```
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[union-attr]
    except (AttributeError, OSError):
        pass

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
HARNESS = "st/probe/probe.hpp"

MAIN_RE = re.compile(r"^auto\s+main\s*\(\s*\)\s*->\s*int\s*\{", re.M)
MAIN_ARGV_RE = re.compile(r"^auto\s+main\s*\(\s*int\s+(\w+)\s*,\s*char\s*\*\*\s*(\w+)\s*\)\s*->\s*int\s*\{", re.M)
# `int main(...)` 形态（本仓两族并存：`auto main() -> int` 与 `int main()`）。
# 两种都是合法写法；外壳只要求签名里拿到 argc/argv，不强制返回类型写法。
MAIN_INT_BARE = re.compile(r"^int\s+main\s*\(\s*\)\s*\{", re.M)
MAIN_INT_ARGV = re.compile(r"^int\s+main\s*\(\s*int\s+(\w+)\s*,\s*char\s*\*\*\s*(\w+)\s*\)\s*\{", re.M)
FONT_EARLY = re.compile(
    r'(?P<indent>[ \t]*)auto\s+(?P<var>\w+)\s*=\s*(?P<expr>[^;]*FontStack::system_default\(\)[^;]*);\s*\n'
    r'(?P=indent)if\s*\(\s*!\s*(?P=var)\s*\)\s*(?:\{\s*\n)?'
    r'(?P=indent)[ \t]*st::print\("(?P<msg>[^"]*)"\);\s*\n'
    r'(?P=indent)[ \t]*return\s+1;\s*\n'
    r'(?P=indent)\}?\s*\n'
)


def find_title(text: str) -> str:
    """取首行注释当标题（86/86 都有文档头，实测）。"""
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("///") or s.startswith("//"):
            body = s.lstrip("/").strip()
            if body and not body.startswith("用法"):
                return body[:80]
        elif s:
            break
    return ""


def transform(name: str, text: str) -> tuple[str, list[str]]:
    """返回 (新文本, 改动说明)。不可改造时抛 ValueError 说明原因。"""
    notes: list[str] = []
    if HARNESS in text:
        raise ValueError("已接外壳")

    has_main = bool(MAIN_RE.search(text)) or bool(MAIN_ARGV_RE.search(text)) or bool(MAIN_INT_BARE.search(text)) or bool(MAIN_INT_ARGV.search(text))
    if not has_main:
        raise ValueError("找不到可识别的 main 入口")

    out = text

    # ① 注入 include（按 §6.3 的包含顺序：本模块头 → 霜天其他头 → 标准库；
    #    probe.hpp 属霜天头，放在 `st/core/print.hpp` 之后最自然）
    if "st/core/print.hpp" in out:
        out = out.replace('#include "st/core/print.hpp"',
                          '#include "st/core/print.hpp"\n#include "st/probe/probe.hpp"', 1)
        notes.append("在 print.hpp 之后插入 probe.hpp")
    else:
        # 无 print.hpp 的探针：插在最后一个 st/ 头之后
        st_includes = list(re.finditer(r'#include "st/[^"]+"', out))
        if st_includes:
            pos = st_includes[-1].end()
            out = out[:pos] + '\n#include "st/probe/probe.hpp"' + out[pos:]
            notes.append("在最后一个 st/ 头之后插入 probe.hpp")
        else:
            raise ValueError("找不到可插入的 st/ 头位置")

    title = find_title(text)
    spec_block = (
        '  const st::probe::spec spec{\n'
        f'      .title = "{_escape(title)}",\n'
        '      .criterion = "",  // 判据由人补（脚本不猜合格线）\n'
        '  };\n'
        '  const auto opts = st::probe::options::from(argc, argv);\n'
        '  if (st::probe::help_requested("' + name + '", spec, opts)) return st::probe::exit_ok;\n'
    )

    # ② 入口签名统一为带 argc/argv（外壳需要它解析 --help/--json）
    if MAIN_ARGV_RE.search(out):
        m = MAIN_ARGV_RE.search(out)
        assert m is not None
        old = m.group(0)
        new = old.replace(m.group(1), "argc").replace(m.group(2), "argv")
        out = out.replace(old, new + "\n" + spec_block, 1)
        notes.append("入口已带 argv，补 spec/options")
    elif MAIN_INT_ARGV.search(out):
        m = MAIN_INT_ARGV.search(out)
        assert m is not None
        old = m.group(0)
        new = old.replace(m.group(1), "argc").replace(m.group(2), "argv")
        out = out.replace(old, new + "\n" + spec_block, 1)
        notes.append("入口 int main(argc,argv) 补 spec/options")
    elif MAIN_INT_BARE.search(out):
        m = MAIN_INT_BARE.search(out)
        assert m is not None
        out = out.replace(m.group(0), "int main(int argc, char** argv) {\n" + spec_block, 1)
        notes.append("入口 int main() 改为带 argc/argv")
    else:
        m = MAIN_RE.search(out)
        assert m is not None
        out = out.replace(m.group(0), "auto main(int argc, char** argv) -> int {\n" + spec_block, 1)
        notes.append("入口签名 auto main() -> 改为带 argc/argv")

    # ③ 条件不具备 ⇒ skip（不是 fail）
    def font_skip(m: re.Match[str]) -> str:
        indent, var, expr, msg = m.group("indent"), m.group("var"), m.group("expr"), m.group("msg")
        return (
            f"{indent}auto {var} = {expr};\n"
            f"{indent}if (!{var}) {{\n"
            f'{indent}  // 「没测成」不是「不合格」：后者会让人以为量规有问题，实际只是本机缺字体。\n'
            f'{indent}  return st::probe::main_of(spec, opts, st::probe::skip("{_escape(msg)}"));\n'
            f"{indent}}}\n"
        )

    out, n = FONT_EARLY.subn(font_skip, out)
    if n:
        notes.append(f"{n} 处「字体缺失 return 1」改为 skip(77)")

    # ④ 结尾 return 0 ⇒ 交给外壳（它按 verdict 决定退出码）
    tail = re.search(r"(\n[ \t]*)return\s+0\s*;\s*\n\}\s*\n?\Z", out)
    if not tail:
        raise ValueError("找不到结尾的 return 0（可能是非标准结构，需人工）")
    out = out[: tail.start()] + (
        f"{tail.group(1)}// 结论退出码由外壳统一给出（0 合格 / 1 不合格 / 77 跳过）\n"
        f"{tail.group(1)}return st::probe::main_of(spec, opts, verdict);\n}}\n"
    )
    notes.append("结尾 return 0 改为 main_of(spec, opts, verdict)")

    # ⑤ 需要 verdict 对象（放在 spec 之后、首个语句之前以免遮蔽）
    if "st::probe::result verdict" not in out:
        anchor = "if (st::probe::help_requested("
        idx = out.find(anchor)
        line_end = out.find("\n", idx)
        out = out[: line_end + 1] + "  st::probe::result verdict{};\n" + out[line_end + 1 :]
        notes.append("补 verdict 对象")

    return out, notes


def _escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true", help="真的写盘（缺省只报）")
    ap.add_argument("--only", default="", help="逗号分隔的探针名（不含 .cpp）")
    args = ap.parse_args(argv)

    only = {s.strip() for s in args.only.split(",") if s.strip()}
    targets = sorted(TOOLS.glob("*.cpp"))
    if only:
        targets = [p for p in targets if p.stem in only]

    done, skipped = 0, 0
    for path in targets:
        text = path.read_text(encoding="utf-8", errors="replace")
        try:
            new, notes = transform(path.stem, text)
        except ValueError as exc:
            print(f"  跳过 {path.name}: {exc}")
            skipped += 1
            continue
        print(f"{'改' if args.apply else '会改'} {path.name}")
        for n in notes:
            print(f"    - {n}")
        if args.apply:
            path.write_text(new, encoding="utf-8")
        done += 1

    print(f"\n{'已改' if args.apply else '可改'} {done} 个，跳过 {skipped} 个")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
