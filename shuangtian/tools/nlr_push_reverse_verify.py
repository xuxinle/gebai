#!/usr/bin/env python3
"""逆向验证：`nlr_push` 必须 `naked`（回退→变红→恢复）。

`CONVENTIONS.md` §7.1：没做过这一步就等于没测。

回退点只有一个（上游那行 `#if !MICROPY_NLR_OS_WINDOWS`），但它必须让**三条**
用例里至少一条变红——否则说明判据没钉住。

预期红灯的形态是**进程崩溃**（`0xC0000005`），不是断言失败：`nlr_push` 写坏
`nlr->regs[0]` 之后，执行流会跳进数据区，测试二进制直接死掉、连汇总行都打不出来。
所以本脚本把“没有汇总行”判为红（理由 `crashed`），而不是“没跑到”。

⚠ 两件必须做对的事（都踩过）：

1. **必须重新链接测试二进制**。只重编对象不够——`st build` 的链接指纹不含
   `third_party_sources`，`nlr_push` 换了实现但链接可能被跳过。
   第一版就因此看到“回退后测试仍是绿的”，实际跑的是旧产物。
   本脚本回退后**先删测试产物再构建**，强制重新链接。
2. **回退写法必须能编过**（§7.3）。这里回退成 `#if !MICROPY_NLR_OS_WINDOWS` 是
   上游原样，能编过，不会出现“编译失败被当成红”的假绿。

用法：python tools/nlr_push_reverse_verify.py
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
NLRX64 = ROOT / "third_party/micropython/py/nlrx64.c"
ST = ROOT / "build/bin/st.exe"
TEST_BINARY = ROOT / "build/debug/bin/st_tests.exe"

# 修复后的形态：无条件启用 naked（只有“老 GCC 退让”那条 #else 分支保留）
FIXED = """#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 8)
#define USE_NAKED 1
#else"""

# 上游原样（Windows 分支既不 naked 也不 omit-frame-pointer）—— 回退目标。
#
# ⚠ 必须带自己的 `#endif`：只把 `#if` 换上去而不加 `#endif`，整份文件会
#   「unterminated #if」编不过——那时脚本会把“编译失败”当成“没有变红”。
#   第一版就这么错过（§7.3 的变体：回退文本编译不过）。
BROKEN = """#if !MICROPY_NLR_OS_WINDOWS
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 8)
#define USE_NAKED 1
#else
// 老 GCC 不支持 `naked` 时的退让：至少掐掉帧指针，让 `(%rsp)` 重新指向返回地址。
__attribute__((optimize("omit-frame-pointer")))
#endif
#endif"""

# 回退目标 = **整段**预处理块（含自己的 #endif），不是只换首行。
#
# ⚠ 只换首行而不管 `#endif`：整份文件会「unterminated #if」编不过，那时脚本会把
#   “编译失败”当成“没有变红”。第一版就这么错过（§7.3 的变体）。
FIXED_BLOCK = """#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 8)
#define USE_NAKED 1
#else
// 老 GCC 不支持 `naked` 时的退让：至少掐掉帧指针，让 `(%rsp)` 重新指向返回地址。
__attribute__((optimize("omit-frame-pointer")))
#endif"""

BROKEN_BLOCK = """#if !MICROPY_NLR_OS_WINDOWS
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 8)
#define USE_NAKED 1
#else
// On older gcc the equivalent here is to force omit-frame-pointer
__attribute__((optimize("omit-frame-pointer")))
#endif
#endif"""

CASES = [
    {
        "name": "恢复上游的 windows 分支（USE_NAKED=0）",
        "old": FIXED_BLOCK,
        "new": BROKEN_BLOCK,
        # 任一条变红都算拦住；三条共用同一根因，单跑一条即可测出。
        "expect": "mp_bpy_prelude_loads",
    },
]

TEST_NAMES = [
    "mp_runtime_runs_a_trivial_script",
    "mp_runtime_survives_python_exception_through_nlr",
    "mp_bpy_prelude_loads",
]


def rebuild_test_binary() -> bool:
    """用 `st test --list` 让**测试构建**走完整路径并拿到最新产物。

    为何不用 `st build --no-tests`：`--no-tests` 直接关掉了测试刷新；即使不加它，
    `refresh_stale_test_binary` 也有一条「产物不存在 → 从没构建过 → 不刷」的早退
    （实测：删掉 `st_tests.exe` 后 `st build --force` 重编了 335 个单元却说
    “链接已跳过（产物最新）”，产物就是不出来）。
    `st test` 的入口判据是「产物为空 → 一律 build_test_binary」，所以它能重建。

    为何删产物：`st build` 的链接指纹不含 `third_party_sources`，改了 `nlrx64.c`
    可能只重编对象、不重链。删产物是唯一能强制重链的可靠手段。
    """
    if TEST_BINARY.exists():
        TEST_BINARY.unlink()
    subprocess.run(
        [str(ST), "test", "--list", "--profile", "debug"],
        cwd=str(ROOT), capture_output=True,
    )
    return TEST_BINARY.exists()


def run_test(name: str) -> tuple[bool, str]:
    result = subprocess.run([str(TEST_BINARY), name], cwd=str(ROOT), capture_output=True)
    raw = (result.stdout or b"") + (result.stderr or b"")
    output = raw.decode("utf-8", errors="replace")
    for match in re.finditer(r"(\d+)\s+passed,\s+(\d+)\s+failed", output):
        if int(match.group(2)) > 0:
            return False, "red"
    if "passed," not in output:
        # 没有汇总行 = 测试进程自己死了 → 真红（本例的预期形态）
        return False, "crashed"
    return result.returncode == 0, "green"


def check_asm(expected_naked: bool) -> tuple[bool, str]:
    """形态判据：`nlr_push` 首条指令是否是 `push %rbp`。"""
    obj = next(
        (p for p in (ROOT / "build/debug/obj").glob("*micropython_py_nlrx64.c.o")), None
    )
    if obj is None:
        return False, "找不到 nlrx64 目标文件"
    disasm = subprocess.run(["objdump", "-d", str(obj)], capture_output=True)
    text = (disasm.stdout or b"").decode("utf-8", errors="replace")
    body = text.split("<nlr_push>:")[-1].splitlines()[1:4] if "<nlr_push>:" in text else []
    joined = "\n".join(body)
    has_push_rbp = "push   %rbp" in joined or "push %rbp" in joined
    if expected_naked and has_push_rbp:
        return False, f"仍有 push %rbp 序言：{joined.strip()}"
    if not expected_naked and not has_push_rbp:
        return False, f"回退后居然没有 push %rbp（回退没生效？）：{joined.strip()}"
    return True, joined.strip().replace("\n", " | ")


def main() -> int:
    failures: list[str] = []
    original = NLRX64.read_text(encoding="utf-8")

    print("=== 基线：修复态 ===")
    if not rebuild_test_binary():
        print("  测试二进制没构建出来")
        return 1
    ok, detail = check_asm(expected_naked=True)
    print(f"  [{'OK' if ok else 'X'}] 形态：nlr_push 无 push %rbp 序言 → {detail}")
    if not ok:
        failures.append("基线形态")
    for name in TEST_NAMES:
        green, why = run_test(name)
        print(f"  [{'OK' if green else 'X'}] {name}: {why}")
        if not green:
            failures.append(f"基线 {name}")

    for case in CASES:
        crlf = "\r\n" in original
        normalized = original.replace("\r\n", "\n")
        if case["old"] not in normalized:
            print(f"[?] 找不到回退点：{case['name']}（源码已改？）")
            failures.append(case["name"])
            continue
        patched = normalized.replace(case["old"], case["new"], 1)
        NLRX64.write_text(patched.replace("\n", "\r\n") if crlf else patched, encoding="utf-8")
        try:
            if not rebuild_test_binary():
                print(f"[?] 回退后构建失败：{case['name']}")
                failures.append(case["name"])
                continue
            ok, detail = check_asm(expected_naked=False)
            print(f"[{'OK' if ok else '?'}] 回退后形态 → {detail}")
            green, why = run_test(case["expect"])
            turned_red = not green
            print(f"[{'OK' if turned_red else 'X'}] 回退「{case['name']}」→ "
                  f"{case['expect']} {'变红' if turned_red else f'**没变红（{why}）**'}")
            if not turned_red:
                failures.append(case["name"])
        finally:
            NLRX64.write_text(original, encoding="utf-8")

    print("\n=== 恢复：必须重新全绿 ===")
    if not rebuild_test_binary():
        print("  恢复后测试二进制没构建出来")
        return 1
    ok, detail = check_asm(expected_naked=True)
    print(f"[{'OK' if ok else 'X'}] 形态恢复 → {detail}")
    if not ok:
        failures.append("恢复形态")
    for name in TEST_NAMES:
        green, why = run_test(name)
        print(f"[{'OK' if green else 'X'}] {name}: {why}")
        if not green:
            failures.append(f"恢复 {name}")

    print()
    if failures:
        print(f"失败 {len(failures)} 项：{failures}")
        return 1
    print("全部通过：修复被逆向验证钉住（回退必崩、恢复必绿）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
