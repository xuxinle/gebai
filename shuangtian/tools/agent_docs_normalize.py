"""把 agent-docs 里所有交叉引用统一成"工具能直接解析"的路径形态，并报告无法解析的引用。

## 为什么需要这一步（而不是靠人记）

文档里的路径有**三种**形态，混着写会让模型的下一跳有一半打不开：

| 形态 | 例 | 工具能否解析 |
|---|---|---|
| 树内相对路径 | `components/input/Slider.md`、`overview/properties.md` | ✅ |
| 相对框架根的旧文档路径 | `DESIGN.md`、`docs/PITFALLS.md` | ✅ |
| 相对当前页的 `../../` 形式 | `../../docs/PITFALLS.md` | ❌（爬出根，被拒绝） |
| 族页里的短形式 | `basic/Text.md` | ❌（少 `components/` 前缀） |

所以脚本做的是**"能解析才算数"的归一化**：对每个写坏的引用，按候选顺序试
（去掉 `../` 前缀 → 加上 `components/` 前缀 → 加上 `docs/agent-docs/` 前缀），
**第一个真能解析的形态就写回去**。这样归一化本身由解析器判定，不靠维护一张映射表。
仍然解析不了的引用**如实报出**（那是真写错了，得人来改），并以退出码 1 结束。

用法（cwd = 仓库根 `shuangtian/`）：

```sh
python tools/agent_docs_normalize.py          # 归一 + 报告
python tools/agent_docs_normalize.py --check  # 只报告，不改（有坏引用退出码 1）
```
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(".").resolve()
TREE = ROOT / "docs" / "agent-docs"
FRAMEWORK = ROOT

REF = re.compile(r"`([^`\n]+\.md)`")


def candidates(ref: str) -> list[str]:
    out: list[str] = []
    stripped = re.sub(r"^(?:\./)+", "", ref)
    stripped = re.sub(r"^(?:\.\./)+", "", stripped)
    for candidate in (ref, stripped, f"components/{stripped}", f"docs/agent-docs/{stripped}"):
        if candidate not in out:
            out.append(candidate)
    return out


def resolve(target: str) -> Path | None:
    """按工具的解析规则试：树内 → 框架根。返回命中的绝对路径。"""
    for root in (TREE, FRAMEWORK):
        base = root.resolve()
        candidate = (base / target).resolve()
        try:
            candidate.relative_to(base)
        except ValueError:
            continue  # 越界
        if candidate.is_file():
            return candidate
        if candidate.is_dir() and (candidate / "index.md").is_file():
            return candidate / "index.md"
    return None


def tool_form(target: str, hit: Path) -> str:
    """把命中文件反算成工具可直接回填的 path 形态（树内相对 / 框架根相对）。"""
    try:
        return hit.relative_to(TREE.resolve()).as_posix()
    except ValueError:
        return hit.relative_to(FRAMEWORK.resolve()).as_posix()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    broken: list[str] = []
    rewrites = 0
    pages_changed = 0
    for page in sorted(TREE.rglob("*.md")):
        text = page.read_text(encoding="utf-8")
        original = text

        def fix(match: re.Match[str]) -> str:
            nonlocal rewrites
            ref = match.group(1).strip()
            if ref.startswith("http") or "*" in ref or "<" in ref:
                return match.group(0)
            for candidate in candidates(ref):
                hit = resolve(candidate)
                if hit is not None:
                    form = tool_form(candidate, hit)
                    if form != ref:
                        rewrites += 1
                    return f"`{form}`"
            return match.group(0)

        text = REF.sub(fix, text)
        if text != original:
            pages_changed += 1
            if not args.check:
                page.write_text(text, encoding="utf-8", newline="\n")

        for m in REF.finditer(text):
            ref = m.group(1).strip()
            if ref.startswith("http") or "*" in ref or "<" in ref:
                continue
            if resolve(ref) is None:
                broken.append(f"{page.relative_to(TREE).as_posix()}: 无法解析 `{ref}`")

    if broken:
        print("[normalize] 无法解析的引用（这些是真写错了，要人改）：")
        for item in broken:
            print(f"  - {item}")
        return 1
    print(f"[normalize] 引用全部可解析；改写 {rewrites} 处 / {pages_changed} 页")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
