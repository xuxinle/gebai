"""逆向验证：把 `Canvas::draw_canvas` 的 blend 修复回退，断言新回归测试**变红**。

为什么必须做这一步：新写的回归测试很可能是「恒绿的假护栏」——它看起像护栏，
实际什么都拦不住。实测踩到过两种形式：

  ① 桩把差异抹平了（参照实现与被测实现共用同一条错误路径）；
  ② 回退文本编译不过（`-Werror` 打断编译 → 跑的是上一份二进制 → 假绿）。

本脚本对 ② 做了防护：回退后**必须**编译成功（回退写法保留所有变量使用），
否则脚本直接报错退出，不会把"编译失败"当成"测试变红"。

用法： icon 无关；本脚本只针对 draw_canvas 的 blend 传递。
"""
import io
import subprocess
import sys
from pathlib import Path

ROOT = Path(r"C:\Users\Administrator\code\gebai\shuangtian")
CANVAS = ROOT / "src/raster/canvas.cpp"
ST = ROOT / "build/dev/bin/st.exe"

FIXED = "      row[x] = blend_pixel_premul(row[x], blended, coverage, options.blend);"
# 回退到修复前的写法。**保留 `coverage` 的使用**（否则 -Wunused-variable 让编译失败，
# 就会得到“假绿”——见脚本头部说明）。
BROKEN = ("      (void)coverage;\n"
          "      row[x] = over_premul(row[x], blended);")

CASES = [
    "canvas_compose_respects_blend_mode",
    "canvas_compose_modes_are_distinct",
    "canvas_scaled_compose_respects_blend_mode",
]


def run_case(name):
    # ⚠ 必须显式 `encoding="utf-8"` + `errors="replace"`：本仓输出是 UTF-8，
    # 而 Windows 上 subprocess 默认用 **GBK** 解码 → 抛 UnicodeDecodeError，
    # 线程里被吞掉后 `stdout` 变 None → 一律报 unknown（实测踩到：
    # 差点把“脚本解码有问题”当成“测试抓不住缺陷”）。
    r = subprocess.run([str(ST), "test", name, "--test-jobs", "1"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace",
                       cwd=ROOT, timeout=900)
    out = (r.stdout or "") + (r.stderr or "")
    if "error:" in out:
        return "compile_error", out
    # 先看汇总行（最可靠），再退回 FAIL 标记。
    if "0 passed, 1 failed" in out:
        return "red", out
    if "1 passed, 0 failed" in out:
        return "green", out
    if "FAIL" in out:
        return "red", out
    return "unknown", out


def main():
    original = CANVAS.read_text(encoding="utf-8")
    if FIXED not in original:
        print("**前置失败**：文件里找不到修复后的那一行，可能已被改动")
        return 2

    print("【回退前】基线（应全绿）")
    for case in CASES:
        status, _ = run_case(case)
        print(f"  {case}: {status}")

    try:
        CANVAS.write_text(original.replace(FIXED, BROKEN), encoding="utf-8")
        print("\n【回退后】期望全部变红（且不得是编译错误）")
        ok = True
        for case in CASES:
            status, out = run_case(case)
            print(f"  {case}: {status}")
            if status == "compile_error":
                print("    **回退写法编译不过**——这是假绿风险，回退文本需要修")
                print("    " + "\n    ".join(out.splitlines()[:6]))
                ok = False
            elif status != "red":
                print(f"    **期望变红但实得 {status}**——这条测试抓不住该缺陷")
                ok = False
        print("\n" + ("逆向验证通过：三条用例都能抓住缺陷" if ok else "**逆向验证失败**"))
        return 0 if ok else 1
    finally:
        CANVAS.write_text(original, encoding="utf-8")
        print("（已恢复修复）")


if __name__ == "__main__":
    sys.exit(main())
