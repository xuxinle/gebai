"""把 prelude + 单幕拼成独立的 layout 脚本，每幕一个文件。

## 为何每幕一个文件（而不是一个文件里按 duration 分派）

第一版按 `Math.abs(D - index) < 0.6` 分发，结果 **4.6s 的幕 1 被判成幕 4**
（`4.6 - 4 = 0.5999…` 在浮点下 < 0.6），而 5.4/6.0 这两个时长还被两幕共用——
分派直接撞车，画面从头到尾是错的，而渲染"成功"了。
每幕一个文件把这件事变成静态的：`layout()` 只调一个函数，没有判据可错。

用法：py build_film.py
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")

# 幕序号 → (源文件, 入口函数)
SCENES = [
    (1, "scene1.js", "scene_converge"),   # 收编
    (2, "scene2.js", "scene_body"),       # 躯体（三维）
    (3, "scene3.js", "scene_tools"),      # 工具面
    (4, "scene4.js", "scene_terminal"),   # 钻穿
    (5, "scene5.js", "scene_field"),      # 扩张
    (6, "scene6.js", "scene_loop"),       # 回路
    (7, "scene7.js", "scene_wordmark"),   # 字标
]


def read(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return f.read()


def main() -> int:
    prelude = read(os.path.join(SRC, "prelude.js"))
    ok = True
    written = []

    for index, filename, entry in SCENES:
        path = os.path.join(SRC, filename)
        if not os.path.exists(path):
            print(f"⚠ 缺文件：src/{filename}", file=sys.stderr)
            ok = False
            continue
        body = read(path)
        if not re.search(rf"function\s+{entry}\s*\(", body):
            print(f"⚠ {filename} 里没有 function {entry}", file=sys.stderr)
            ok = False
            continue

        dispatch = (
            f"\n// 本幕入口（每幕一个 layout 文件：没有判据可错）\n"
            f"function layout(t, duration, canvas, layers) {{\n"
            f"  return {entry}(t, duration, canvas, layers);\n"
            f"}}\n"
        )
        text = prelude + "\n\n" + body + dispatch
        out = os.path.join(HERE, f"gbfilm-s{index}.js")
        with open(out, "w", encoding="utf-8") as f:
            f.write(text)
        written.append((out, len(text)))

    for out, size in written:
        print(f"写入 {os.path.basename(out)}  ({size} 字符)")

    # 语法检查（.cjs：仓库 package.json 是 ESM，写 require 会报错）
    smoke = r"""
const fs = require('fs');
let bad = 0;
for (const p of process.argv.slice(2)) {
  const src = fs.readFileSync(p, 'utf8');
  try { new Function(src); } catch (e) { bad++; console.log('SYNTAX FAIL', p, e.message); }
}
console.log(bad === 0 ? 'syntax ok (' + (process.argv.length - 2) + ' files)' : 'syntax FAILED');
process.exit(bad === 0 ? 0 : 1);
"""
    tmp = os.path.join(HERE, ".syntax_check.cjs")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(smoke)
    try:
        files = [os.path.join(HERE, f"gbfilm-s{i}.js") for i, _, _ in SCENES]
        r = subprocess.run(["node", tmp] + files, capture_output=True, text=True, timeout=120)
        print((r.stdout + r.stderr).strip())
        if r.returncode != 0:
            ok = False
    except FileNotFoundError:
        print("（无 node，跳过语法检查）")
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
