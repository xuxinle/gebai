"""逆向验证：把 rotate 的实现改坏，确认 fx 的几何测试真会变红。

## 为什么必须做（CONVENTIONS §7.1 / §7.4）

不做这一步的回归测试很容易变成「恒绿测试」——看起来像护栏，实际什么都拦不住。
本轮**五次**实操全部是**判定链路**而不是断言的问题，值得完整记下来：

| # | 做法 | 结果 | 真因 |
|---|---|---|---|
| 1 | 靠 `st test` 的 returncode | 四条回退全"仍绿" | 它无论结果如何都返回 1 |
| 2 | 解析 stdout 的 `FAIL` 行 | 仍全绿 | 见 #4 |
| 3 | 先 `st build` 再 `st test` | 仍全绿 | **`st build` 不构建测试单元**（`st test` 才构建） |
| 4 | `st build --force`（重编 147） | 仍全绿 | 同上：147 个单元里没有测试单元 |
| 5 | 删掉测试单元的 `.o` 再 build | "重编 0/命中缓存 147" | 同上 |

**正解：`st test <用例名>` 本身就是构建入口**，它会把测试单元重编再跑。
唯一的坑是：改的若是**被 include 进测试的 .cpp**（本项目大量如此：`#include "fx.cpp"`），
`st test` 的增量判据只看测试 `.cpp` 的 mtime——所以脚本每轮都**touch 一下测试文件**
逼它重编，这比删缓存可靠。

另外，跑单一用例（`st test <name>`）比跑全套快得多（4.6s vs 数十秒）。

运行：py scripts/verify_rotate_reverse.py
"""

import glob
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TARGET = os.path.join(ROOT, "examples", "caiyun", "fx.cpp")
TEST_SRC = os.path.join(ROOT, "tests", "caiyun_fx_test.cpp")
BACKUP = os.path.join(HERE, "_fx_backup.cpp")
ST = os.path.join(ROOT, "build", "bin", "st.exe")
OBJ_DIR = os.path.join(ROOT, "build", "dev", "obj")
TESTS_BIN = os.path.join(ROOT, "build", "dev", "bin", "st_tests.exe")

# (名字, 原文, 改坏成什么, 期望变红的用例名)
CASES = [
    (
        "rotate 完全失效（退化成恒等）",
        "  if (degrees == 0.0f) return point;",
        "  return point;  // 回退：rotate 静默不生效",
        "fx_rotated_square_bbox_grows_and_area_is_conserved",
    ),
    (
        "旋转方向反了（符号错）",
        # 要把**两项**都改，才是真正的逆时针：
        #   顺时针：x' = dx·c - dy·s , y' = dx·s + dy·c
        #   逆时针：x' = dx·c + dy·s , y' = -dx·s + dy·c
        # 只改一项时 (100,0) 这类 dy=0 的点根本不受影响（-0 ≡ +0），
        # 断言自然不变红——这是逆向验证自己写错了回退点（已踩三次）。
        "  return st::math::Point{center.x + dx * c - dy * s, center.y + dx * s + dy * c};",
        "  return st::math::Point{center.x + dx * c + dy * s, center.y + dy * c - dx * s};",
        "fx_rotate_quarter_turn_is_clockwise",
    ),
    (
        "旋转时顺手缩了一点（双重缩放类错误）",
        "  return st::math::Point{center.x + dx * c - dy * s, center.y + dx * s + dy * c};",
        "  return st::math::Point{center.x + (dx * c - dy * s) * 0.98f,"
        " center.y + (dx * s + dy * c) * 0.98f};",
        "fx_rotate_point_keeps_distance_to_center",
    ),
    (
        "旋转中心用了顶点均值而不是包围盒中心",
        "  const st::math::Point center{(min_x + max_x) * 0.5f, (min_y + max_y) * 0.5f};",
        "  st::math::Point center{0.0f, 0.0f};\n"
        "  for (std::size_t i = 0; i < count; ++i) {"
        " center.x += points[i].x; center.y += points[i].y; }\n"
        "  center.x /= static_cast<float>(count); center.y /= static_cast<float>(count);",
        "fx_rotate_about_bounds_keeps_bbox_center_fixed",
    ),
]


def force_test_rebuild() -> None:
    """逼测试单元重编。

    ## 为什么不能只 touch 源文件（本轮返工六次的主因）

    测试 `#include` 了 `fx.cpp`，而构建系统的快路径判据是 `.o.meta` 里记的
    (mtime, size) 双校验。理论上它该能抓到“内容变了”，但**实测抓不到**：
    注入回退后（文件变短 11 字节、mtime 也推到未来）仍然命中缓存。

    回退后又恢复成原大小，`.meta` 里的 (mtime, size) 就与现状重新一致，
    于是构建认为“没变”——跑的还是上一份二进制，四条回退全报“仍绿”。

    所以这里直接删除测试单元的中间产物，使它无法命中。可靠、可重复，
    比猜构建系统的判据便宜得多。
    """
    for pattern in ("*caiyun_fx_test*.o", "*caiyun_fx_test*.o.d",
                    "*caiyun_fx_test*.o.flags", "*caiyun_fx_test*.o.meta"):
        for f in glob.glob(os.path.join(OBJ_DIR, pattern)):
            try:
                os.remove(f)
            except OSError:
                pass
    # 还必须删测试可执行文件本身：只删 `.o` 时链接可能复用旧产物
    # （实测：删 `.o` 后仍绿，连 `st_tests.exe` 一起删才变红）。
    try:
        os.remove(TESTS_BIN)
    except OSError:
        pass


def run_case(case: str) -> bool:
    """跑该用例，返回它是否**失败**。

    ## 判定方式踩过两个坑

    1. **不能用 returncode**：`st test` 无论结果如何都可能返回 1
       （日志落 stderr 的包装行为）。
    2. **不能用 `(\\d+) failed` 汇总行**：只跑单个用例时，输出里**没有汇总行**，
       只有一行 `FAIL <name>`（带 ANSI 颜色码）。用汇总正则判断会永远得到
       “失败数 0”——四条回退因此全报“仍绿”（本轮第 6 次返工）。

    正确做法：**去 ANSI 后找 `FAIL <用例名>`**。
    """
    force_test_rebuild()
    r = subprocess.run([ST, "test", case, "--profile", "dev"], cwd=ROOT,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = (r.stdout or "") + (r.stderr or "")
    plain = re.sub(r"\x1b\[[0-9;]*m", "", out)   # 去 ANSI 颜色码
    return re.search(rf"^\s*FAIL\s+{re.escape(case)}\b", plain, re.MULTILINE) is not None


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    for path in (TARGET, TEST_SRC):
        if not os.path.exists(path):
            print(f"✗ 找不到 {path}")
            return 1

    base_cases = [c[3] for c in CASES]
    bad0 = [c for c in base_cases if run_case(c)]
    if bad0:
        print(f"✗ 基线状态下已有失败用例：{bad0}")
        return 1
    print("基线：全绿 ✓\n")

    shutil.copy2(TARGET, BACKUP)
    src = open(TARGET, encoding="utf-8").read()
    not_caught = []
    try:
        for name, old, new, expect in CASES:
            if old not in src:
                print(f"⚠ 回退点未命中，跳过：{name}")
                not_caught.append(name)
                continue
            open(TARGET, "w", encoding="utf-8").write(src.replace(old, new, 1))
            failed = run_case(expect)
            print(f"{'✓' if failed else '✗'} 改坏「{name}」→ "
                  f"{'变红（' + expect + '）' if failed else '仍绿'}")
            if not failed:
                not_caught.append(name)
    finally:
        shutil.copy2(BACKUP, TARGET)
        os.remove(BACKUP)

    bad1 = [c for c in base_cases if run_case(c)]
    print(f"\n恢复后：{'全绿 ✓' if not bad1 else f'仍红 ✗ {bad1}'}")
    if bad1:
        return 1
    if not_caught:
        print(f"\n✗ {len(not_caught)} 条回退未被测试拦住：{not_caught}")
        return 1
    print("全部回退都被拦住了——测试是真护栏，不是恒绿摆设")
    return 0


if __name__ == "__main__":
    sys.exit(main())
