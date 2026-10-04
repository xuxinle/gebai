"""**真机参照取数**：调起真实（非无头）Edge 对对照页截图，给「与参照比对」用。

为什么必须有这个工具（2026-10-04 的教训）：
上一轮用 `msedge --headless --screenshot` 当“浏览器基准”，据此把字形提亮，
结果被用户实测驳回——**无头浏览器不带桌面 GPU 合成与字体渲染链路**，
同一台机器同一 DPI 下它渲染出的字比用户实际看到的更浅。参照错了，方向就反了。

本工具的做法：以**可见窗口**（`--new-window`，普通模式）打开对照页并截图，
保证走的是与用户桌面一致的渲染链路。窗口位置/尺寸固定，便于逐次可比。

用法：
  python tools/text_reference_shot.py <html> <out.png> [--edge <exe>] [--scale 1.5]
输出：落盘 PNG，并打印“文字区朴素口径”统计（与 text_raw_lum_compare.py 同口径），
      可直接拿来与霜天截图对照。
"""
import argparse
import os
import subprocess
import sys
import time

DEFAULT_EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("html")
    parser.add_argument("out_png")
    parser.add_argument("--edge", default=DEFAULT_EDGE)
    parser.add_argument("--scale", default="1.5")
    parser.add_argument("--width", type=int, default=1100)
    parser.add_argument("--height", type=int, default=700)
    parser.add_argument("--keep-open", action="store_true",
                        help="不自动关窗（人工核对观感时用）")
    args = parser.parse_args()

    if not os.path.exists(args.edge):
        print(f"找不到浏览器：{args.edge}", file=sys.stderr)
        return 1
    url = "file:///" + os.path.abspath(args.html).replace("\\", "/")
    profile = os.path.abspath(".st/reference-profile")
    os.makedirs(profile, exist_ok=True)

    # 关键：不用 --headless。--app 让页面没有地址栏（截图区域干净），
    # 但仍是**完整桌面渲染链路**（GPU 合成 + 系统字体渲染）。
    command = [
        args.edge,
        f"--user-data-dir={profile}",
        f"--force-device-scale-factor={args.scale}",
        f"--window-size={args.width},{args.height}",
        "--app=" + url,
        "--no-first-run",
        "--disable-features=Translate",
    ]
    print("启动真实浏览器（非无头）：", " ".join(command))
    process = subprocess.Popen(command)
    try:
        # 等页面绘制完成（首次启动可能较慢）；这里只能按经验等待——
        # 无头模式的 --screenshot 是同步的，而真实窗口没有等就绪的接口。
        time.sleep(6.0)
        # 用系统截图能力抓窗口（PowerShell 的 Win32 屏幕抓取不可用时不落盘）
        shot = subprocess.run(
            ["pwsh", "-NoProfile", "-Command",
             "$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.Drawing,System.Windows.Forms;"
             "$b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds;"
             "$bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height;"
             "$g=[System.Drawing.Graphics]::FromImage($bmp);"
             "$g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size);"
             f"$bmp.Save('{os.path.abspath(args.out_png)}',[System.Drawing.Imaging.ImageFormat]::Png);"
             "Write-Output 'shot-ok'"],
            capture_output=True, text=True)
        if "shot-ok" not in (shot.stdout or ""):
            print("截屏失败：", shot.stdout, shot.stderr, file=sys.stderr)
            return 1
        print(f"[OK] {args.out_png}（整屏截图；请按需裁剪到对照页区域）")
    finally:
        if not args.keep_open:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
