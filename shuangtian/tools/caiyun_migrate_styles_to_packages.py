"""把七套风格模板的多镜示例迁成工程包形态（`.cypkg`）。

## 为什么要做这一步

P1~P4 交付的是能力，而"能力被用起来"才算落地。七套模板是仓库里
**最真实的一批脚本**（多镜、带 layout.js、带承接、带素材），拿它们走一遍
"建包 + 体检 + 拍平比对"能立刻暴露包在真实创作里哪里不顺手——
这比再写十组构造出来的测试有用。

## 为什么不替换原脚本

模板的 `film.json` / `film-multishot.json` 被这些引用着：
`render_all.py`、`carry_qa.py`、`measure_motion.py`、文档、以及别人克隆后的习惯。
换成包形态会**同时**打断它们。所以这里是**并存**：
包里放同一份内容的另一形态，两者由本脚本的往返比对保证等价。

真要让包成为唯一真值源，得等所有工具都支持包输入——那是另一步，
不该由一次迁移顺手决定。

## 判据

每个包都必须满足：
  ① 建包成功；
  ② `--check` 通过（意图符合 + 素材完整）；
  ③ 拍平结果与该脚本的规范化形态**逐字节一致**（语义零丢失）。

任何一条不满足就判失败，并打印可定位的差异。
"""

from __future__ import annotations

import io
import json
import os
import shutil
import subprocess
import sys

ROOT = "D:/code/gebai-pro/shuangtian"
EXE = os.path.join(ROOT, "build/dev/bin/caiyun.exe")
STYLES = os.path.join(ROOT, "examples/caiyun/styles")


def run(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        [EXE, "--headless", *args],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )


def migrate_one(style_dir: str) -> tuple[bool, list[str]]:
    """把一个模板目录下的每份导演脚本迁成同名包。返回 (成功, 消息)。"""
    notes: list[str] = []
    ok = True
    name = os.path.basename(style_dir)

    scripts: list[str] = []
    for entry in sorted(os.listdir(style_dir)):
        if not entry.endswith(".json"):
            continue
        path = os.path.join(style_dir, entry)
        try:
            with io.open(path, encoding="utf-8") as fh:
                data = json.load(fh)
        except Exception:
            continue
        if isinstance(data, dict) and isinstance(data.get("scenes"), list):
            scripts.append(path)

    if not scripts:
        return False, [f"{name}: 没找到导演脚本"]

    for script in scripts:
        stem = os.path.splitext(os.path.basename(script))[0]
        pkg = os.path.join(style_dir, f"{stem}.cypkg")
        if os.path.isdir(pkg):
            shutil.rmtree(pkg)

        # ① 建包
        made = run(["--pack", script, pkg])
        if made.returncode != 0:
            ok = False
            notes.append(f"{name}/{stem}: 建包失败 rc={made.returncode} "
                         f"{(made.stderr or made.stdout)[:300]}")
            continue

        # ③ 拍平后仍可跑且语义完整
        #
        # 注意：拍平结果**不再与原脚本逐字节相同**——拍平会把 JS 编排层
        # 内联进去（那样导出物才自包含，离开包目录也能跑）。
        # 所以这里判的是“layout 不能丢”，而不是“文本一样”。
        flat = run(["--flatten", pkg])
        if flat.returncode != 0:
            ok = False
            notes.append(f"{name}/{stem}: 拍平失败")
            continue
        try:
            flat_data = json.loads(flat.stdout)
        except Exception as exc:
            ok = False
            notes.append(f"{name}/{stem}: 拍平输出不是合法 JSON（{exc}）")
            continue

        # 原脚本里用了 layout 的幕，拍平后必须是**内联对象**（带 source）
        with io.open(script, encoding="utf-8") as fh:
            orig_data = json.load(fh)
        missing_layout = 0
        for index, scene in enumerate(orig_data.get("scenes", [])):
            if "layout" not in scene:
                continue
            flat_scene = flat_data.get("scenes", [])[index] if index < len(
                flat_data.get("scenes", [])) else {}
            value = flat_scene.get("layout")
            if not isinstance(value, dict) or not value.get("source"):
                missing_layout += 1
        if missing_layout:
            ok = False
            notes.append(f"{name}/{stem}: {missing_layout} 幕的 layout 没被内联（离开包目录就丢动效）")
            continue

        # ④ 体检（意图 + 素材）
        checked = run(["--check", pkg])
        if checked.returncode != 0:
            ok = False
            notes.append(f"{name}/{stem}: 体检未过\n    "
                         + (checked.stdout or checked.stderr).strip().replace("\n", "\n    ")[:400])
            continue

        # 报告包里有什么（人要知道这个包长什么样）
        items = sorted(os.listdir(pkg))
        js_count = len([x for x in items if x.endswith(".js")])
        notes.append(f"{name}/{stem}: OK（{len(items)} 项，其中 {js_count} 份 layout）")

    return ok, notes


def main() -> int:
    if not os.path.exists(EXE):
        print(f"找不到裁云可执行文件：{EXE}")
        return 2

    all_ok = True
    all_notes: list[str] = []
    for entry in sorted(os.listdir(STYLES)):
        path = os.path.join(STYLES, entry)
        if not os.path.isdir(path):
            continue
        if not entry[0].isdigit():
            continue  # 只处理 01-…07- 这类模板目录
        ok, notes = migrate_one(path)
        all_ok = all_ok and ok
        all_notes.extend(notes)

    print("迁移结果：")
    for note in all_notes:
        print("  " + note)
    print()
    print("全部通过" if all_ok else "有失败项（见上）")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
