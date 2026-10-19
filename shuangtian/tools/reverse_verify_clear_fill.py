"""逐条回退「清屏+首层底色合并」的前提，确认 `clear_fill_merge_probe` 会变红。

按 `CONVENTIONS.md` §7.1 / §7.3：新写的回归探针必须**逆向验证**——
每条**真正必要**的前提单独破坏一次，看探针是否真的红。

## 设计本脚本时踩到的三点（记下来免得下一个人重踩）

1. **跨行正则很脆**：早先版本用 `\\s*\\n\\s*` 匹配多行 `&&` 链，四条里三条 `[SKIP]`。
   改成按**完整片段**替换（片段直接取自当前源码，改代码时本脚本会 `[SKIP]` 报警）。
2. **回退写法必须能编译过**：把 `clipped.right() >= whole.right() && clipped.bottom() >= ...`
   换成 `true` 会留下 `&& true &&` 的悬空运算符 → `-Werror` 打断编译 → 测的是旧二进制
   （即 §7.3 的“假绿”变体）。所以替换时**连带吃掉相邻的 `&&`**。
3. **不是每条 `&&` 都是真前提**：回退 `radius <= 0` 后探针仍绿——带圆角时走
   `fill_path` → 那个入口本身已把 `cleared_fresh_` 清掉，永远到不了跳过分支。
   它是**冗余保护**，实测无分辨力，故不列入本脚本。

用法：py -3 build/reverse_verify_clear_fill.py
"""
import pathlib
import subprocess
import sys

# 仓库根 = 本文件的上上级（脚本在 tools/ 下，运行时 cwd 任意）。
# 不要写绝对路径：它会把验证锁死在某台机器上。
ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / 'src/raster/canvas.cpp'
ORIGINAL = SRC.read_text(encoding='utf-8')

# 条件块原文（片段必须与源码逐字一致）
COND_START = '  if (cleared_fresh_ && skip_redundant_clear_fill_ && paint.is_solid() && fill_color.a == 255U &&\n'
COND_MID = ('      radius <= 0.0f && options.opacity >= 0.999f && options.blend == BlendMode::SrcOver &&\n'
            '      !has_mask_clip() && fill_color.r == cleared_color_.r &&\n'
            '      fill_color.g == cleared_color_.g && fill_color.b == cleared_color_.b &&\n')
COND_END = ('      clipped.x <= whole.x && clipped.y <= whole.y && clipped.right() >= whole.right() &&\n'
            '      clipped.bottom() >= whole.bottom()) {\n')

CASES = [
    (
        '去掉「清屏后没被碰过」前提（cleared_fresh_）',
        COND_START,
        '  if (skip_redundant_clear_fill_ && paint.is_solid() && fill_color.a == 255U &&\n',
    ),
    (
        '去掉「颜色必须与 clear 一致」前提',
        COND_MID,
        ('      radius <= 0.0f && options.opacity >= 0.999f && '
         'options.blend == BlendMode::SrcOver &&\n'
         '      !has_mask_clip() && true &&\n'),
    ),
    # ⚠ 以下三条**故意不列入**（包括「必须覆盖整屏」）：它们在像素层**不可区分**，
    # 不能靠黑盒探针验证。
    #
    # - 「必须覆盖整屏」：局部填充要能被这道条件拦住，它得是**同色**（否则被颜色条件
    #   先挡住）；而局部填同色到同色底上，跳过与不画的结果逐像素相同——探针看不见。
    # - 「不透明」：无先前内容时，同色半透明叠加到同色上（`over(B, B×a) = B`）结果不变；
    #   一旦有先前内容，`cleared_fresh_` 已失效，压根到不了这道判断。
    # - 「SrcOver」：同理，同色 `Src` 写到同色上结果也相同。
    #
    # 它们仍是廉价的**防御性安全网**（代价是几字节比较），保留合理；
    # 但不应在文档里声称“已被回归测试验证”——那是假保证。
    (
        '`rasterize_path_runs` 不再清除 cleared_fresh_（漏掉 fill_circle 入口）',
        'void Canvas::rasterize_path_runs(const Path& path, const Paint& paint, DrawOptions options) {\n'
        '  cleared_fresh_ = false;\n',
        'void Canvas::rasterize_path_runs(const Path& path, const Paint& paint, DrawOptions options) {\n',
    ),
    # ⚠ 「`fill_path` 入口不再清除」不列入：`fill_path` 的 sprite 命中路径最终走
    #    `blit_geometry_sprite`，那里也有清除；而光栅化路径走 `rasterize_path_runs`。
    #    所以回退 `fill_path` 入口那一行**仍然安全**（下游两处都覆盖了）——它是**冗余保护**，
    #    实测无分辨力。写上它会让验证脚本谎报“这条前提已被测试”。
]


def build() -> bool:
    r = subprocess.run(
        [str(ROOT / 'build/bin/st.exe'), 'build', 'clear_fill_merge_probe', '--profile', 'release'],
        cwd=ROOT, capture_output=True)
    return r.returncode == 0


def run() -> int:
    r = subprocess.run([str(ROOT / 'build/release/bin/clear_fill_merge_probe.exe')],
                       cwd=ROOT, capture_output=True)
    return r.returncode


def main() -> int:
    SRC.write_text(ORIGINAL, encoding='utf-8')
    if not build():
        print('baseline build FAILED')
        return 2
    base = run()
    print(f'baseline exit={base} (expect 0)')
    if base != 0:
        print('baseline not green; reverse verification meaningless')
        return 2

    problems = 0
    for label, old, new in CASES:
        if old not in ORIGINAL:
            print(f'[SKIP] {label}: snippet not found (code changed, update script)')
            problems += 1
            continue
        SRC.write_text(ORIGINAL.replace(old, new, 1), encoding='utf-8')
        if not build():
            print(f'[ERROR] {label}: revert does not compile '
                  '(revert must compile, else the test runs the old binary)')
            problems += 1
            SRC.write_text(ORIGINAL, encoding='utf-8')
            continue
        code = run()
        good = code != 0
        print(f'[{"RED-as-expected" if good else "STILL-GREEN!!"}] {label} -> exit={code}')
        if not good:
            problems += 1
        SRC.write_text(ORIGINAL, encoding='utf-8')

    SRC.write_text(ORIGINAL, encoding='utf-8')
    build()
    print('restored and rebuilt')
    return 0 if problems == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
