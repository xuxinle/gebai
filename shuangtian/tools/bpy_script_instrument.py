"""在脚本的顶层语句之间插入 `result = 'MARK n'`，用它定位 MicroPython 失败点。

## 为什么需要

MicroPython MINIMUM 档报 `NameError: name not defined` **不说是哪个名字**，
也没有行号。400 行的脚本靠猜没戏。

插入标记后：执行到第 k 段失败时，宿主读回的 `result` 还是 `MARK k-1`，
于是"哪一段开始失败"就确定了。

## 关键：切点必须是**顶层语句边界**

按行硬切会切坏 docstring/多行表达式 → 得到无关的 SyntaxError，
第一次实现就是这样（把目标指向了 docstring 中间，白跑一轮）。

所以这里真的跟踪三引号状态与括号深度。

## 用法

    py tools/bpy_script_instrument.py <脚本.py> > /tmp/instrumented.py
"""

import sys


def top_level_statement_starts(lines):
    """返回顶层语句起始行号（0 基）列表。"""
    starts = []
    in_triple = None
    paren_depth = 0
    backslash_continuation = False

    for index, line in enumerate(lines):
        stripped = line.strip()

        if in_triple is not None:
            # 统计本行是否闭合三引号
            if stripped.count(in_triple) % 2 == 1:
                in_triple = None
            continue

        if backslash_continuation:
            backslash_continuation = stripped.endswith('\\')
            continue

        if not stripped or stripped.startswith('#'):
            continue

        is_top = (line[:1] not in (' ', '\t')) and paren_depth == 0
        if is_top:
            starts.append(index)

        # 更新状态（粗略但够用）
        for quote in ('"""', "'''"):
            if stripped.count(quote) % 2 == 1:
                in_triple = quote
                break
        if in_triple is None:
            for ch in stripped:
                if ch in '([{':
                    paren_depth += 1
                elif ch in ')]}':
                    paren_depth = max(0, paren_depth - 1)
            if paren_depth > 0 and stripped.endswith('\\'):
                backslash_continuation = True
    return starts


def main():
    if len(sys.argv) < 2:
        print('用法：py tools/bpy_script_instrument.py <脚本.py>', file=sys.stderr)
        return 2
    path = sys.argv[1]
    with open(path, encoding='utf-8') as handle:
        lines = handle.read().split('\n')

    starts = top_level_statement_starts(lines)
    out = []
    last = 0
    for ordinal, start in enumerate(starts):
        out.extend(lines[last:start])
        out.append("result = 'MARK %d/%d'" % (ordinal, len(starts)))
        last = start
    out.extend(lines[last:])
    sys.stdout.write('\n'.join(out))
    print('已插 %d 个标记（原始 %d 行 → %d 行）' % (len(starts), len(lines), len(out)),
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
