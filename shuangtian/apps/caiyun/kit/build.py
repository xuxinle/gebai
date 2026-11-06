#!/usr/bin/env python3
"""把 caiyun-kit 与你的 layout 脚本拼成一份可被裁云 eval 的单文件。

## 为什么需要这一步

裁云的 `layout` 是**单文件整体 eval**（`LayoutEngine::load` → `engine.eval(source)`），
脚本引擎没有模块系统：`import` / `require` 都不可用。

所以复用只能走"预处理器"：渲染前把 kit 正文拼到脚本前面。你的脚本仍是普通单文件、
仍能独立读懂，而通用能力（缓动 / 相机 / 背景纹理 / 图元工厂）不必复制粘贴。

## 用法

    py kit/build.py --script my-film.js --out _build/my-film.bundle.js

    # 或者让 build.py 直接改 json 里的 layout 字段（--patch-json）
    py kit/build.py --script my-film.js --out _build/my-film.bundle.js \\
                    --patch-json my-film.json

## 它做的三件事（都很朴素，刻意不引入打包器）

1. 拼：`kit` 正文 + 你的脚本正文。之间留一行注释标出边界。
2. 查：拼完之后过一遍括号/引号配平和禁用词（`Math.random` / `import` / `require`）。
   裁云的脚本引擎出错时会**静默降级为纯 JSON**，画面只剩背景而无报错——
   所以语法与禁用词必须在**进渲染器之前**拦住。
3. 报：打印行号偏移（你的脚本报错时，行号要减掉 kit 的行数才能对上）。
"""

import argparse
import json
import os
import re
import sys

KIT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "caiyun-kit.js")

# 裁云禁止的东西：出现即失败（不是警告）
FORBIDDEN = [
    (re.compile(r"\bMath\.random\b"), "Math.random（裁云要求逐帧可复现，用 hash01 代替）"),
    (re.compile(r"^\s*import\s", re.MULTILINE), "import（脚本引擎无模块系统，用 build.py 拼接）"),
    (re.compile(r"\brequire\s*\("), "require（同上）"),
    (re.compile(r"\bDate\.now\b|\bperformance\.now\b"), "时间源（会破坏确定性）"),
]


def strip_js_comments(src: str) -> str:
    """粗略去掉行注释与块注释（只为检查配平，不追求完美）。"""
    out = []
    i = 0
    n = len(src)
    in_s = None      # 当前字符串定界符
    while i < n:
        c = src[i]
        if in_s:
            out.append(c)
            if c == "\\":
                if i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
            elif c == in_s:
                in_s = None
            i += 1
            continue
        if c in "\"'`":
            in_s = c
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def check_balance(src: str, label: str) -> list[str]:
    """括号 / 引号配平检查。返回问题清单（空 = 通过）。"""
    clean = strip_js_comments(src)
    pairs = {")": "(", "]": "[", "}": "{"}
    stack = []
    in_s = None
    i = 0
    n = len(clean)
    problems = []
    while i < n:
        c = clean[i]
        if in_s:
            if c == "\\":
                i += 2
                continue
            if c == in_s:
                in_s = None
            i += 1
            continue
        if c in "\"'`":
            in_s = c
            i += 1
            continue
        if c in "([{":
            stack.append((c, i))
        elif c in ")]}":
            if not stack:
                problems.append(f"{label}: 多余的 {c}")
                return problems
            op, pos = stack.pop()
            if op != pairs[c]:
                problems.append(f"{label}: {op} 与 {c} 不匹配")
                return problems
        i += 1
    if in_s:
        problems.append(f"{label}: 字符串未闭合（{in_s}）")
    if stack:
        problems.append(f"{label}: {len(stack)} 个括号未闭合（最后一个是 {stack[-1][0]}）")
    return problems


def main() -> int:
    # Windows 控制台默认 GBK：中文会变成乱码（U+FFFD），后续排查容易误判。
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--script", required=True, help="你的 layout 脚本（只写业务，不写 kit）")
    ap.add_argument("--out", required=True, help="拼接产物路径")
    ap.add_argument("--patch-json", default="", help="顺手把 json 里的 layout 改成产物路径")
    ap.add_argument("--no-kit", action="store_true", help="只检查不拼（脚本自带全部依赖时）")
    args = ap.parse_args()

    kit_src = "" if args.no_kit else open(KIT, encoding="utf-8").read()
    body = open(args.script, encoding="utf-8").read()

    # ① 禁用词（在拼之前查，否则 kit 自己的注释也可能命中）
    bad = []
    for line_no, line in enumerate(body.split("\n"), start=1):
        for pattern, why in FORBIDDEN:
            if pattern.search(line):
                bad.append(f"{args.script}:{line_no}: {why}")
    if bad:
        print("禁用特性：", file=sys.stderr)
        for b in bad:
            print("  " + b, file=sys.stderr)
        return 2

    # ② 配平（拼之后查总长，能同时发现 kit 与脚本的交接问题）
    bundle = (kit_src + "\n" if kit_src else "") + body
    problems = check_balance(bundle, "bundle")
    if problems:
        print("语法结构问题：", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 2

    # ③ 写产物
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    header = (
        "// ==== 本文件由 kit/build.py 生成，请勿手改 ====\n"
        f"// kit: {os.path.basename(KIT)}（{kit_src.count(chr(10)) + 1} 行）\n"
        f"// body: {os.path.basename(args.script)}（{body.count(chr(10)) + 1} 行）\n"
        f"// 你的脚本报错时，行号减去 {kit_src.count(chr(10)) + 5} 才是 body 的行号\n"
        "// ================================================\n"
    )
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(header + bundle)
    offset = kit_src.count("\n") + 4
    print(f"已生成 {args.out}")
    print(f"  kit {kit_src.count(chr(10))+1} 行 + body {body.count(chr(10))+1} 行")
    print(f"  body 报错时行号 - {offset}")

    # ④ 可选：改 json 的 layout 指向产物
    if args.patch_json:
        data = json.load(open(args.patch_json, encoding="utf-8"))
        target = os.path.relpath(os.path.abspath(args.out),
                                 os.path.dirname(os.path.abspath(args.patch_json)))
        target = target.replace("\\", "/")
        hit = False
        for scene in data.get("scenes", []):
            if "layout" in scene:
                scene["layout"] = target
                hit = True
        if hit:
            json.dump(data, open(args.patch_json, "w", encoding="utf-8"),
                      ensure_ascii=False, indent=2)
            print(f"  已把 {args.patch_json} 的 layout 指向 {target}")
        else:
            print(f"  警告：{args.patch_json} 里没有 scene.layout，未修改", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
