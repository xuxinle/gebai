"""pyregex 子代理项目专属工具（经 driver.py 框架加载合并）。

演示 Python 基础框架的项目扩展点：本文件与 manifest 同目录，driver.py 启动时自动加载——
导出 AGENT_NAME（覆盖项目名推导）与 TOOLS/TOOL_IMPLS（与基础工具合并，同名覆盖）。
项目只写工具逻辑，协议/REPL/pip 等基础能力由语言目录共享的 driver.py 提供。
"""

import re

AGENT_NAME = "pyregex"  # manifest name 一致（driver.py 已按目录名推导，此处显式声明可读性更好）

_TOOLS = [
    {
        "name": "match",
        "description": "正则匹配测试：pattern 对 text 全文搜索（re.search），返回是否命中与全部分组"
        "（命名分组用 dict 返回）。常驻进程适合反复调试正则。",
        "parameters": {
            "type": "object",
            "properties": {
                "pattern": {"type": "string", "description": "正则表达式（Python re 语法）"},
                "text": {"type": "string", "description": "待匹配文本"},
                "flags": {
                    "type": "string",
                    "description": "标志位（可选，逐字符组合）：i=IGNORECASE m=MULTILINE s=DOTALL x=VERBOSE",
                },
            },
            "required": ["pattern", "text"],
        },
    },
    {
        "name": "findall",
        "description": "正则批量提取：返回全部命中（有分组返回分组元组）。pattern 对 text 反复扫描。",
        "parameters": {
            "type": "object",
            "properties": {
                "pattern": {"type": "string", "description": "正则表达式"},
                "text": {"type": "string", "description": "待扫描文本"},
                "limit": {"type": "number", "description": "返回上限（默认 100，防超长输出）"},
                "flags": {"type": "string", "description": "同 match 的标志位"},
            },
            "required": ["pattern", "text"],
        },
    },
    {
        "name": "sub",
        "description": "正则替换：pattern 对 text 全部替换为 replacement（支持 \\1、\\g<name> 反向引用）。",
        "parameters": {
            "type": "object",
            "properties": {
                "pattern": {"type": "string", "description": "正则表达式"},
                "replacement": {"type": "string", "description": "替换模板（支持反向引用）"},
                "text": {"type": "string", "description": "待替换文本"},
                "count": {"type": "number", "description": "替换次数上限（默认全部）"},
                "flags": {"type": "string", "description": "同 match 的标志位"},
            },
            "required": ["pattern", "replacement", "text"],
        },
    },
]

_FLAG_MAP = {"i": re.IGNORECASE, "m": re.MULTILINE, "s": re.DOTALL, "x": re.VERBOSE}


def _compile(pattern, flags=""):
    fl = 0
    for ch in str(flags or ""):
        fl |= _FLAG_MAP.get(ch, 0)
    return re.compile(pattern, fl)


def _tool_match(args):
    try:
        rx = _compile(args["pattern"], args.get("flags"))
    except re.error as exc:
        return {"output": f"正则编译失败: {exc}", "data": {"ok": False}}
    m = rx.search(str(args.get("text") or ""))
    if not m:
        return {"output": "未命中", "data": {"ok": True, "matched": False}}
    data = {"ok": True, "matched": True, "match": m.group(0)}
    named = m.groupdict()
    if named:
        data["named"] = named
    groups = [g if g is not None else None for g in m.groups()]
    if groups:
        data["groups"] = groups
    lines = [f"命中: {m.group(0)!r}", f"位置: {m.start()}-{m.end()}"]
    if named:
        lines.append("命名分组: " + ", ".join(f"{k}={v!r}" for k, v in named.items()))
    if groups:
        lines.append("分组: " + ", ".join(repr(g) for g in groups))
    return {"output": "\n".join(lines), "data": data}


def _tool_findall(args):
    try:
        rx = _compile(args["pattern"], args.get("flags"))
    except re.error as exc:
        return {"output": f"正则编译失败: {exc}", "data": {"ok": False}}
    limit = int(args.get("limit") or 100)
    hits = rx.findall(str(args.get("text") or ""))
    total = len(hits)
    shown = hits[:limit]
    lines = [f"共 {total} 处命中" + (f"（仅显示前 {limit}）" if total > limit else "") + ":"]
    for h in shown:
        lines.append(f"  {h!r}")
    data = {"ok": True, "count": total, "hits": [list(h) if isinstance(h, tuple) else h for h in shown]}
    return {"output": "\n".join(lines), "data": data}


def _tool_sub(args):
    try:
        rx = _compile(args["pattern"], args.get("flags"))
    except re.error as exc:
        return {"output": f"正则编译失败: {exc}", "data": {"ok": False}}
    count = int(args.get("count") or 0)
    text = str(args.get("text") or "")
    result, n = rx.subn(str(args.get("replacement") or ""), text, count if count > 0 else 0)
    preview = result if len(result) <= 2000 else result[:2000] + f"…（共 {len(result)} 字符）"
    return {"output": f"替换 {n} 处:\n{preview}", "data": {"ok": True, "count": n, "result": result}}


TOOLS = _TOOLS
TOOL_IMPLS = {"match": _tool_match, "findall": _tool_findall, "sub": _tool_sub}
