"""批量修复：覆写 get_property / set_property 但未回退基类的组件。

判据（精确，不靠正则猜）：
  找到 `auto <T>::get_property(...) const -> std::optional<std::string> {` 的函数体，
  在花括号配对范围内，若正文里没有 `Element::get_property`，就把**函数末尾**
  的 `return std::nullopt;` 换成 `return Element::get_property(name);`。
  同名 `set_property` 同理把末尾 `return false;` 换成 `return Element::set_property(name, value);`

安全性：只改“函数体最后一个 return 语句”且该 return 是兜底值的情形；
不用全文替换（避免误伤函数中间的分支 return）。
"""

import re
import sys
from pathlib import Path

ROOT = Path(r"C:\Users\Administrator\code\gebai\shuangtian\src\ui\components")

# 函数头：auto X::get_property(std::string_view name) const -> std::optional<std::string> {
GET_HEAD = re.compile(
    r"auto\s+(\w+)::get_property\(std::string_view\s+(\w+)\)\s*const\s*->\s*std::optional<std::string>\s*\{"
)
SET_HEAD = re.compile(
    r"auto\s+(\w+)::set_property\(std::string_view\s+(\w+),\s*std::string_view\s+(\w+)\)\s*->\s*bool\s*\{"
)


def find_body(text: str, open_brace: int) -> int:
    """返回匹配 `}` 的下标（花括号配对，忽略字符串/字符字面量里的括号）。"""
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


def patch(text: str, head_re, fallback_old: str, fallback_new_tpl: str, base_call: str):
    """返回 (新文本, 修改处数)。"""
    changes = 0
    pos = 0
    while True:
        m = head_re.search(text, pos)
        if not m:
            break
        body_start = m.end() - 1
        body_end = find_body(text, body_start)
        if body_end < 0:
            pos = m.end()
            continue
        body = text[body_start:body_end]
        if base_call in body:
            pos = body_end
            continue  # 已经回退过
        # 找函数体**最后一个** fallback_old
        idx = body.rfind(fallback_old)
        if idx < 0:
            pos = body_end
            continue
        # 只在它后面没有其它语句（即它是兜底）时才改：
        tail = body[idx + len(fallback_old):].strip()
        if tail not in ("", "}", "//"):
            pos = body_end
            continue
        new_body = body[:idx] + fallback_new_tpl + body[idx + len(fallback_old):]
        text = text[:body_start] + new_body + text[body_end:]
        changes += 1
        pos = body_start + len(new_body)
    return text, changes


def main() -> int:
    total_get = 0
    total_set = 0
    touched = []
    for path in sorted(ROOT.glob("*.cpp")):
        src = path.read_text(encoding="utf-8-sig")
        original = src
        src, n_get = patch(
            src, GET_HEAD, "return std::nullopt;",
            "return Element::get_property(name);",
            "Element::get_property",
        )
        src, n_set = patch(
            src, SET_HEAD, "return false;",
            "return Element::set_property(name, value);",
            "Element::set_property",
        )
        if src != original:
            path.write_text(src, encoding="utf-8")
            touched.append((path.name, n_get, n_set))
            total_get += n_get
            total_set += n_set
    print(f"改了 {len(touched)} 个文件：get_property {total_get} 处，set_property {total_set} 处")
    for name, g, s in touched:
        print(f"  {name}: get={g} set={s}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
