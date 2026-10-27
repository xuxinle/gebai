"""存量脚本兼容性验证：把每份 `film.json` 走一遍「建包 → 拍平」，比对逐字节一致。

## 为什么用脚本而不是加一条 C++ 用例

存量脚本是**会变的**（作者一直在改），把它们的内容硬编码进测试用例会立刻过期。
这里用"扫目录 + 逐个比对"的方式：新增脚本自动纳入，不需要维护清单。

## 判据

`flatten(create(script)) == script_to_json(parse(script))`

注意参照物是 `script_to_json(parse(x))` 而不是 `x` 原文——因为包拍平出来的是
**规范形态**（键序固定、缩进统一），而存量文件的排版各不相同。
要验的是"语义没丢"，不是"格式没变"。
"""

from __future__ import annotations

import io
import json
import os
import subprocess
import sys
import tempfile

ROOT = "D:/code/gebai-pro/shuangtian"
EXE = os.path.join(ROOT, "build/dev/bin/caiyun.exe")


def run(args: list[str]) -> subprocess.CompletedProcess:
    """跑一次裁云（无界面模式），统一 捕获输出 + UTF-8 解码。"""
    return subprocess.run(
        [EXE, "--headless", *args],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )


def find_scripts() -> list[str]:
    out: list[str] = []
    base = os.path.join(ROOT, "examples/caiyun")
    for dirpath, dirnames, filenames in os.walk(base):
        # 跳过**包目录**：包里的 `scene.json` 也是“顶层有 scenes 的 JSON”，
        # 但它是**产物**不是源。把它当源会去 pack 一个已经拍平过的文件，
        # 而拍平内联过的 layout 不是文件引用，再 pack 一次就会丢。
        dirnames[:] = [d for d in dirnames if not d.endswith(".cypkg")]
        for name in filenames:
            if not name.endswith(".json"):
                continue
            # 素材库索引与资源清单不是导演脚本
            if name in ("library.json",):
                continue
            path = os.path.join(dirpath, name)
            # 只认"顶层有 scenes 数组"的才是导演脚本
            try:
                with io.open(path, encoding="utf-8") as fh:
                    data = json.load(fh)
            except Exception:
                continue
            if isinstance(data, dict) and isinstance(data.get("scenes"), list):
                out.append(path)
    return sorted(out)


def main() -> int:
    scripts = find_scripts()
    print(f"发现 {len(scripts)} 份导演脚本")
    if not scripts:
        print("没有脚本可验——断言失效，直接判失败")
        return 1

    ok = 0
    failed: list[tuple[str, str]] = []
    tmp_root = tempfile.mkdtemp(prefix="cypkg_legacy_")

    for idx, script in enumerate(scripts):
        pkg = os.path.join(tmp_root, f"p{idx}.cypkg")
        # ① 用包转换（--pack）建包
        r = subprocess.run(
            [EXE, "--headless", "--pack", script, pkg],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        if r.returncode != 0:
            failed.append((script, f"建包失败 rc={r.returncode}: {(r.stderr or r.stdout)[:300]}"))
            continue
        # ③ 拍平后仍可跑且语义完整
        #
        # 注意：拍平结果**与原脚本不再逐字节相同**——拍平会把 JS 编排层
        # 内联进去（那样导出物才自包含，离开包目录也能跑）。
        # 所以判据分两种：
        #   · 脚本没有 layout → 应当逐字节相同（纯拍平往返保真）；
        #   · 脚本有 layout   → 拍平必须把它变成带 source 的内联对象
        #     （否则离开包目录就丢动效），且**其它字段仍与原文一致**。
        flat = run(["--flatten", pkg])
        if flat.returncode != 0:
            failed.append((script, f"拍平失败 rc={flat.returncode}"))
            continue
        try:
            flat_data = json.loads(flat.stdout)
        except Exception as exc:
            failed.append((script, f"拍平输出不是合法 JSON：{exc}"))
            continue

        with io.open(script, encoding="utf-8") as fh:
            orig_data = json.load(fh)

        has_layout = any("layout" in scene for scene in orig_data.get("scenes", []))
        if not has_layout:
            # 无 layout：拍平应与规范化原脚本逐字节相同
            r3 = subprocess.run(
                [EXE, "--headless", "--normalize", script],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            if r3.returncode != 0:
                failed.append((script, f"规范化失败 rc={r3.returncode}"))
                continue
            if flat.stdout != r3.stdout:
                a, b = flat.stdout, r3.stdout
                limit = min(len(a), len(b))
                d = 0
                while d < limit and a[d] == b[d]:
                    d += 1
                failed.append((script, f"拍平与原文不一致，首差异 offset {d}"))
                continue
            ok += 1
            continue

        # 有 layout：比对“去掉 layout 后”的文本，并校验 layout 已内联
        def strip_layout(doc: dict) -> str:
            clone = json.loads(json.dumps(doc))
            for scene in clone.get("scenes", []):
                scene.pop("layout", None)
            return json.dumps(clone, sort_keys=True, ensure_ascii=False)

        # 参照物必须是**规范化后的**原脚本，不是原始文本：
        # 原脚本里写 `0.35` / `#0a0d16`，而包的序列化会写成
        # `0.3499999940395355` / `#0a0d16ff`（float32 位精确、颜色补 alpha）。
        # 直接拿原文比会把“规范化”误判成“信息丢失”。
        norm = run(["--normalize", script])
        if norm.returncode != 0:
            failed.append((script, f"规范化失败 rc={norm.returncode}"))
            continue
        normalized_orig = json.loads(norm.stdout)

        missing = 0
        for scene in flat_data.get("scenes", []):
            if "layout" not in scene:
                continue
            value = scene["layout"]
            if not isinstance(value, dict) or not value.get("source"):
                missing += 1
        if missing:
            failed.append((script, f"{missing} 幕的 layout 没被内联"))
            continue

        if strip_layout(flat_data) != strip_layout(normalized_orig):
            failed.append((script, "除 layout 外的字段与原文不一致（规范化后）"))
            continue
        ok += 1

    print(f"\n通过 {ok} / {len(scripts)}")
    if failed:
        print(f"\n失败 {len(failed)} 份：")
        for path, why in failed:
            print(f"  - {os.path.relpath(path, ROOT)}")
            print(f"    {why}")
        return 1
    print("全部一致")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
