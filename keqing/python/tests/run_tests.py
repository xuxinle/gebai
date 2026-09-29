#!/usr/bin/env python3
"""客卿 Python 侧的测试运行器（纯标准库，无 pytest 依赖）。

为什么需要它：`keqing/python/**` 此前没有任何测试，Python 边车工具（vision / docqa / imgproc）
的改动只能靠临时脚本验证，回归无处沉淀——改一次、验一次、下次重来。

约定（与 TS 侧对齐的最小集合）：
- 测试文件：本目录下 `*_test.py`（与 TS 的 `*.test.ts` 同构）
- 用例：模块内 `test_*` 可调用（pytest 风格命名，但不依赖 pytest）
- 依赖缺失的用例**跳过而非失败**：Python 侧的 AI 依赖（numpy/onnxruntime）是可选的，
  缺它们不该让 CI 变红（`skip("原因")` 显式跳过并计入 skips）

由 `packages/server/src/core/agents/keqing-python.test.ts` 经 bun test 包装调用，
也可直接 `python3 keqing/python/tests/run_tests.py` 单跑。
"""

from __future__ import annotations

import importlib.util
import inspect
import os
import sys
import traceback

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
# 客卿 Python 语言目录（keqing/python/）——驱动与各子代理项目在其二级目录
LANG_DIR = os.path.dirname(TESTS_DIR)
# 仓库根（keqing/ 的上两级）
REPO_ROOT = os.path.dirname(os.path.dirname(LANG_DIR))


class Skip(Exception):
    """显式跳过（依赖缺失等），不算失败。"""


def skip(reason: str) -> None:
    raise Skip(reason)


def load_module(path: str, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载模块: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def discover() -> list[str]:
    return sorted(
        os.path.join(TESTS_DIR, entry)
        for entry in os.listdir(TESTS_DIR)
        if entry.endswith("_test.py")
    )


def main() -> int:
    files = discover()
    if not files:
        print("[python-tests] 未发现 *_test.py")
        return 0

    passed = failed = skipped = 0
    failures: list[str] = []
    for path in files:
        module_name = os.path.basename(path)[: -len(".py")]
        print(f"\n=== {module_name} ===")
        try:
            module = load_module(path, f"keqing_test_{module_name}")
        except Skip as exc:  # 整模块级跳过
            skipped += 1
            print(f"  SKIP  {module_name}: {exc}")
            continue
        except Exception:  # 模块导入失败本身就是失败（不许静默）
            failed += 1
            failures.append(f"{module_name}: 模块导入失败")
            print(f"  FAIL  {module_name}: 模块导入失败")
            traceback.print_exc()
            continue

        for name, function in sorted(inspect.getmembers(module, inspect.isfunction)):
            if not name.startswith("test_"):
                continue
            try:
                function()
            except Skip as exc:
                skipped += 1
                print(f"  SKIP  {name}: {exc}")
            except Exception as exc:
                failed += 1
                failures.append(f"{module_name}::{name}: {exc}")
                print(f"  FAIL  {name}: {exc}")
                traceback.print_exc()
            else:
                passed += 1
                print(f"  PASS  {name}")

    print(f"\n{passed} passed, {failed} failed, {skipped} skipped")
    if failures:
        print("\n失败用例：")
        for item in failures:
            print(f"  - {item}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
