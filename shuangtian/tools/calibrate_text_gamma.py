"""覆盖率 gamma 标定：**一条命令重跑完整流程**（浅底 + 深底）。

存在的理由：`kDefaultCoverageGamma` / `kDefaultCoverageGammaOnDark` 的当前值是用
真窗口浏览器标定的。换 DPI、换字体、换后端之后这些值**需要重标**——而重标流程
涉及"生成对照页 → 起真窗口浏览器 → 摆窗口 → 截图 → 逐字号扫 gamma → 插值求根"
七步，散着做必然漏步或口径漂移。本脚本把可自动化的部分固定下来。

## 分工（**必须人机各做一半**）

  · 脚本自动：生成对照页、编译探针、渲染霜天侧全部 (gamma × 字号)、量尺与插值
  · **需要你手动**：起真窗口浏览器并把它摆到屏幕上、截图
    —— 因为"把浏览器窗口摆到前台且不被遮挡"依赖当前桌面状态
    （实测：`Start-Process` 起的浏览器会被已有全屏窗口盖住，自动化点自绘 UI 菜单也不可靠）。

脚本会打印**你要执行的确切命令**（浏览器启动参数 + 截图工具），你跑完把截图路径喂回来。

用法：
  # 第一步：生成对照页、渲染霜天侧、打印待办
  python tools/calibrate_text_gamma.py prepare --theme light
  # 第二步：按打印的提示起浏览器并截图，然后
  python tools/calibrate_text_gamma.py measure --theme light --ref <截图.png>
"""

import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROBE = ROOT / "build" / "probe" / "size_line_probe.exe"
OUT = ROOT / "build" / "probe" / "calib"

# 两主题各扫的字号（**须与 gen_realwin_page.py 的框顺序一致**）
SIZES = [10, 11, 12, 14, 16]
# 两主题的扫描档位：理想值都在区间内（浅 ~1.1 / 深 ~0.6）
GAMMAS = {"light": ["060", "075", "090", "105", "120"],
          "dark": ["040", "050", "060", "070", "085"]}
# 主题 token（`src/ui/theme.cpp`）——探针与对照页必须同源
COLORS = {"light": ("FFFFFF", "0F172A"), "dark": ("0A0F1A", "E8EEF9")}


def browser_cmd(theme: str) -> str:
    """打印真窗口浏览器的启动命令（EDGE/CHROME 都可，用独立 profile 避免继承登录态）。"""
    page = ("sizes_boxed_dark.html" if theme == "dark" else "sizes_boxed.html")
    url = f"file:///{(ROOT / 'build' / 'probe' / 'realwin' / page).as_posix()}"
    return (f'"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe" '
            f'--user-data-dir=%LOCALAPPDATA%\\ChromeRealWin --no-first-run '
            f'--no-default-browser-check --disable-session-crashed-bubble --disable-infobars '
            f'--window-size=700,1050 --window-position=100,20 '
            f'--force-device-scale-factor=1.5 --hide-scrollbars --force-color-profile=srgb '
            f'"{url}"')


def prepare(theme: str) -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    bg, fg = COLORS[theme]
    dark_flag = ["--dark"] if theme == "dark" else []

    gen = [sys.executable, str(ROOT / "tools" / "gen_realwin_page.py"), *dark_flag,
           *[str(s) for s in SIZES]]
    print("① 生成对照页:", " ".join(gen))
    subprocess.run(gen, check=True, cwd=ROOT)

    print("② 编译探针（若无）")
    subprocess.run(["pwsh", "-NoProfile", "-File", str(ROOT / "tools" / "build_probes.ps1"),
                    "-Only", "size_line_probe"], check=True, cwd=ROOT, capture_output=True)

    st_dir = OUT / theme
    st_dir.mkdir(parents=True, exist_ok=True)
    print(f"③ 渲染霜天侧：{len(GAMMAS[theme])} 档 γ × {len(SIZES)} 档字号")
    for g in GAMMAS[theme]:
        value = g[:1] + "." + g[1:]
        for size in SIZES:
            subprocess.run([str(PROBE), str(st_dir / f"{g}.{size}.png"), "1.5", value,
                            str(size), f"--bg={bg}", f"--fg={fg}"],
                           check=True, cwd=ROOT, capture_output=True)

    print(f"④ 霜天侧已就绪：{st_dir}")
    print()
    print("=" * 78)
    print("**接下来需要你手动做**（依赖当前桌面状态，无法可靠自动化）：")
    print("=" * 78)
    print("\nA. 起真窗口浏览器（这一步是为了拿到**非无头**参照——无头不做 ClearType")
    print("   调校、默认灰度抗锯齿，标出来的值不适用真机）：\n")
    print("   " + browser_cmd(theme))
    print("\nB. 让窗口完整可见（6 个框要全在屏内），然后截图。")
    print("   可用桌面工具截图，或直接按 Win+Shift+S。把 PNG 存到任意路径。")
    print("\nC. 回来跑量尺：\n")
    print(f"   python tools/calibrate_text_gamma.py measure --theme {theme} --ref <截图.png>")
    return 0


def measure(theme: str, ref: str) -> int:
    st_dir = OUT / theme
    cmd = [sys.executable, str(ROOT / "tools" / "realwin_ink.py"), ref, str(st_dir),
           "{g}.{s}.png", "--sizes", *[str(s) for s in SIZES],
           "--gammas", *GAMMAS[theme]]
    if theme == "dark":
        cmd.append("--dark")
    print("量尺:", " ".join(str(c) for c in cmd))
    print()
    subprocess.run(cmd, check=True, cwd=ROOT)
    print()
    print("把上表的『理想 γ』取中位数，与该主题的默认常量对比：")
    print("  · 浅底 → `TextRenderer::kDefaultCoverageGamma`")
    print("  · 深底 → `TextRenderer::kDefaultCoverageGammaOnDark`")
    print("若差距 > 5%，更新常量并同步 DESIGN.md（§4.3.7.17 / §4.3.7.18 各自的表）。")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="覆盖率 gamma 标定（浅底/深底）")
    ap.add_argument("action", choices=["prepare", "measure"])
    ap.add_argument("--theme", choices=["light", "dark"], required=True)
    ap.add_argument("--ref", help="measure 用：真窗口浏览器截图路径")
    args = ap.parse_args()
    if args.action == "prepare":
        return prepare(args.theme)
    if not args.ref:
        print("measure 需要 --ref <截图.png>")
        return 1
    return measure(args.theme, args.ref)


if __name__ == "__main__":
    raise SystemExit(main())
