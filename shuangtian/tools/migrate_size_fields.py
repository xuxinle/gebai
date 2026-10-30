#!/usr/bin/env python3
"""把 JSON 里的旧尺寸字段（radius/width/height/depth）换算成 `size`。

## 为什么需要工具而不是人手改

测试与示例里有 68+ 处旧写法，分散在 C++ raw string 里。手改有两个风险：
换错单位（radius 是半长、width 是全长——两种语义混在一起）和漏改
（漏一处就是一条红测试，而红的原因看起来像"实现坏了"）。

## 换算规则（与 `build_mesh` 的旧语义逐条对齐）

| 形状 | 旧字段 → size |
|---|---|
| cube/box | width→size[0], depth→size[1] 且 size[2]（旧语义里 height 无效） |
| plane | width→size[0], height→size[1] |
| 球类（sphere/uv_sphere/ico_sphere/正多面体） | radius×2 → size（标量） |
| cylinder/cone/capsule/prism/gear/tube/helix/arrow | radius×2→size[0], depth 或 height→size[1] |
| torus/torus_knot | radius 是中心线半径：外径=2×(r+0.35r), 管径=2×0.35r |
| 其它 | radius×2→size[0], depth→size[1] |

用法：
    py tools/migrate_size_fields.py <文件...>          # 预览
    py tools/migrate_size_fields.py --apply <文件...>  # 写入
"""
import json
import re
import sys

SPHERE = {"sphere", "uv_sphere", "ico_sphere", "tetrahedron", "octahedron",
          "dodecahedron", "icosahedron", "disc", "circle"}
CYLINDER = {"cylinder", "cone", "capsule", "prism", "gear", "tube", "helix", "arrow"}
TORUS = {"torus", "torus_knot"}


def convert_object(obj):
    """把一个对象里的旧尺寸字段换算成 size。返回是否有改动。"""
    shape = obj.get("shape")
    if not isinstance(shape, str):
        return False
    old = {k: obj[k] for k in ("radius", "width", "height", "depth") if k in obj}
    if not old:
        return False
    if "size" in obj:
        return False  # 已经用新写法

    radius = old.get("radius")
    width = old.get("width")
    height = old.get("height")
    depth = old.get("depth")

    if shape in ("cube", "box"):
        # 旧实测：width→X，depth→Y 且 Z；height 完全无效
        x = width if width is not None else (radius * 2 if radius is not None else 1.0)
        y = depth if depth is not None else 1.0
        z = depth if depth is not None else 0.0
        size = [x, y] if not z else [x, y, z]
    elif shape == "plane":
        x = width if width is not None else (radius * 2 if radius is not None else 1.6)
        z = height if height is not None else (depth if depth is not None else 1.6)
        size = [x, z]
    elif shape in SPHERE:
        size = (radius * 2) if radius is not None else 1.0
    elif shape in TORUS:
        r = radius if radius is not None else 0.8
        outer = 2.0 * (r + 0.35 * r)
        tube = 2.0 * 0.35 * r
        size = [round(outer, 6), round(tube, 6)]
    elif shape in CYLINDER:
        d = radius * 2 if radius is not None else 1.0
        h = depth if depth is not None else (height if height is not None else 1.0)
        size = [d, h]
    else:
        d = radius * 2 if radius is not None else 1.0
        h = depth if depth is not None else (height if height is not None else 1.0)
        size = [d, h]

    for key in ("radius", "width", "height", "depth"):
        obj.pop(key, None)
    obj["size"] = size
    return True


def walk(node, changed):
    if isinstance(node, dict):
        if "shape" in node and convert_object(node):
            changed.append(dict(node))
        for value in node.values():
            walk(value, changed)
    elif isinstance(node, list):
        for value in node:
            walk(value, changed)


def migrate_json_text(text):
    """在 JSON 文本里就地替换（保留原格式）。"""
    changed = []
    try:
        doc = json.loads(text)
    except Exception:
        return text, changed
    walk(doc, changed)
    if not changed:
        return text, changed
    # 重新序列化会丢格式，所以只在确实有改动时整段替换
    return json.dumps(doc, ensure_ascii=False, indent=2), changed


def process_file(path, apply_change):
    src = open(path, encoding="utf-8", errors="ignore").read()
    total = []
    # 逐个 raw string 段落处理（C++ 里的 R"j( ... )j"）
    # 同时匹配 `R"j(...)j"` 与 `R"(...)"` 两种定界符
    pattern = re.compile(r'R"(?:j)?\((.*?)\)(?:j)?"', re.S)

    def repl(match):
        body = match.group(1)
        new_body, changed = migrate_json_text(body)
        total.extend(changed)
        return f'R"j({new_body})j"'

    out = pattern.sub(repl, src)
    if total and apply_change:
        open(path, "w", encoding="utf-8").write(out)
    return total


def main():
    args = [a for a in sys.argv[1:] if a != "--apply"]
    apply_change = "--apply" in sys.argv
    grand = 0
    for path in args:
        changed = process_file(path, apply_change)
        if changed:
            grand += len(changed)
            print(f"{path}: {len(changed)} 个物体")
            for obj in changed[:3]:
                print(f"    -> {json.dumps(obj, ensure_ascii=False)[:110]}")
    print(f"\n共 {grand} 个物体{'（已写入）' if apply_change else '（预览，加 --apply 写入）'}")


if __name__ == "__main__":
    main()
