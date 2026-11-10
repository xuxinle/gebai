"""批量给覆写 `property_names` / `readonly_property_names` 的组件包上 `with_universal`。

判据（精确）：
  找到 `auto X::property_names() const -> std::vector<std::string_view> {` 的函数体，
  若正文里没有 `with_universal`，就把**函数末尾的 return 表达式**整体包一层：
      return {...};                      →  return Element::with_universal({...});
  只处理“唯一的那个 return”（有多 return 分支的跳过并报告，人工看）。

安全性：用花括号配对 + 只在函数体范围内动；不碰其它函数。
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(r"C:\Users\Administrator\code\gebai\shuangtian\src\ui\components")

HEAD_RE = re.compile(
    r"auto\s+(\w+)::(property_names|readonly_property_names)\(\)\s*const\s*"
    r"->\s*std::vector<std::string_view>\s*\{"
)

# 基类名：判据是 `property_names` 定义在 `Element` 上。组件都直接/间接继承，
# C++ 里 `Element::with_universal(...)` 一定可达（static 成员不需对象）。
BASE = "Element"


def find_body(text: str, open_brace: int) -> int:
    depth = 0
    i = open_brace
    while i < len(text):
        ch = text[i]
        if ch == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif ch == "'":
            i += 1
            while i < len(text) and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
        elif ch == "/" and i + 1 < len(text) and text[i + 1] == "/":
            while i < len(text) and text[i] != "\n":
                i += 1
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def wrap_return(body: str) -> str | None:
    """把函数体里唯一的 `return <expr>;` 包成 `return Element::with_universal(<expr>);`。"""
    if body.count("return") != 1:
        return None
    m = re.search(r"return\s+(.+?);", body, re.S)
    if not m:
        return None
    expr = m.group(1).strip()
    wrapped = f"return {BASE}::with_universal({expr});"
    return body[: m.start()] + wrapped + body[m.end():]


def main() -> int:
    touched, skipped = [], []
    for path in sorted(ROOT.glob("*.cpp")):
        src = path.read_text(encoding="utf-8-sig")
        original = src
        pos = 0
        while True:
            m = HEAD_RE.search(src, pos)
            if not m:
                break
            body_start = m.end() - 1
            body_end = find_body(src, body_start)
            if body_end < 0:
                pos = m.end()
                continue
            body = src[body_start:body_end]
            if "with_universal" in body:
                pos = body_end
                continue
            new_body = wrap_return(body)
            if new_body is None:
                skipped.append(f"{path.name}:{m.group(1)}::{m.group(2)}")
                pos = body_end
                continue
            src = src[:body_start] + new_body + src[body_end:]
            touched.append(f"{path.name}:{m.group(1)}::{m.group(2)}")
            pos = body_start + len(new_body)
        if src != original:
            path.write_text(src, encoding="utf-8")
    print(f"包了 {len(touched)} 处")
    for item in touched:
        print(f"  {item}")
    if skipped:
        print(f"\n跳过 {len(skipped)} 处（多 return 分支，需人工看）：")
        for item in skipped:
            print(f"  {item}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
