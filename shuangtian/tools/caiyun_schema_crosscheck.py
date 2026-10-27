"""用标准 JSON Schema 校验器交叉验证裁云导出的导演脚本 schema。

## 为什么需要这个脚本（而不是只靠 C++ 单测）

实测代价（2026-10-27）：`schema_to_json_schema` 的 C++ 单测**六条全绿**，
因为测试断言的是"结构长什么样"（有没有 oneOf、有没有 additionalProperties）。
而用标准校验器一跑就发现两处致命缺口：

1. `oneOf` 里被包成了数组的数组（`[{"type":"number"}]` 而非 `{"type":"number"}`）
   —— 合法 JSON，但不符合 draft-2020-12 的元 schema；
2. `layers` 只声明了 `"type":"array"` 而**没有 items**，于是校验器不会进入元素，
   图层里的拼写错（`model_rot`）**永远抓不到**。schema 看着很完整，三个样本全放行。

这两条都是"自己的测试以为对、别人读起来不对"。标准校验器是唯一能证伪的口径。

## 用法

    # 先把运行中应用的导出落盘（控制通道 get director json_schema）
    python tools/caiyun_schema_crosscheck.py <schema.json> [<样本目录>]

退出码 0 = 全部符合预期；1 = 有缺口（会打印具体路径与消息）。
"""

import io
import json
import os
import sys

try:
    import jsonschema
    from jsonschema import Draft202012Validator
except ImportError:
    print("需要 jsonschema：pip install jsonschema")
    sys.exit(2)


# 三个样本：分别钉住「合法通过」「拼写错被拒」「类型错被拒」。
SAMPLES = [
    ("合法脚本", True, {
        "name": "t", "fps": 30, "size": [320, 180],
        "scenes": [{"kind": "custom", "duration": 1}],
    }),
    ("字段名拼错 model_rot", False, {
        "name": "t",
        "scenes": [{"kind": "custom", "duration": 1,
                    "layers": [{"kind": "rect", "model_rot": 5}]}],
    }),
    ("类型写错 opacity", False, {
        "name": "t",
        "scenes": [{"kind": "custom", "duration": 1,
                    "layers": [{"kind": "rect", "opacity": "high"}]}],
    }),
    ("Dimension 用百分比串（合法）", True, {
        "name": "t",
        "scenes": [{"kind": "custom", "duration": 1,
                    "layers": [{"kind": "rect", "x": "10%"}]}],
    }),
]


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    schema = json.load(io.open(path, encoding="utf-8"))

    # ① schema 自身是否合法（这条就能抓住 oneOf 嵌套错）
    try:
        Draft202012Validator.check_schema(schema)
    except jsonschema.SchemaError as exc:
        print("[FAIL] schema 自身不合法:", exc.message)
        return 1
    print("[OK  ] schema 通过 draft-2020-12 元 schema 校验")

    validator = Draft202012Validator(schema)
    failed = 0
    for label, should_pass, doc in SAMPLES:
        errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.path))
        passed = not errors
        ok = passed == should_pass
        if not ok:
            failed += 1
        print(f"[{'OK  ' if ok else 'FAIL'}] {label}: "
              f"{'通过' if passed else '被拒'}（期望{'通过' if should_pass else '被拒'}）")
        for err in errors[:3]:
            where = "/".join(str(p) for p in err.path) or "(root)"
            print(f"         {where}: {err.message[:160]}")

    print()
    if failed:
        print(f"{failed} 个样本不符预期——schema 与解析器语义不一致")
        return 1
    print("全部符合预期")
    return 0


if __name__ == "__main__":
    sys.exit(main())
