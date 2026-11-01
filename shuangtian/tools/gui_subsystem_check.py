#!/usr/bin/env python3
"""验证「链接期输入」（`system` / `icon`）改动**真的**进到产物里。

## 为什么需要这个脚本

`link_fingerprint` 决定“链接能不能跳过”。它最初只算：编译器族 + 对象清单 +
编译标志 + 每个对象的内容哈希。**`system` 与 `.ico` 不在其中**——于是：

    st.pkg 里把 system 从 console 改成 gui
    → 单元一个没变 → 指纹不变 → "链接已跳过" → 产物还是 CONSOLE
    → 构建输出却写着"完成"

实测代价：给 7 个桌面应用统一改 gui 后，`caiyun.exe` 的 PE 头仍是 CONSOLE，
只有 `--force` 才生效。换图标同理（`.rc` 只看路径，路径没变就不重编资源）。

## 判据（都是**读产物**，不是读构建输出）

1. `system: gui` → PE Optional Header 的 Subsystem == 2（WINDOWS_GUI）；
2. `system: console` → == 3；
3. 换 `.ico` → 产物里 256×256 的那块 PNG 指纹必须变。

用法：
    py -3 tools/gui_subsystem_check.py            # 全部应用
    py -3 tools/gui_subsystem_check.py caiyun     # 单个
    py -3 tools/gui_subsystem_check.py --reverse  # 逆向验证（逐项回退→必须变红）
"""
import hashlib
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ST = ROOT / "build" / "bin" / ("st.exe" if sys.platform == "win32" else "st")
PKG = ROOT / "st.pkg"
PROFILE = "dev"

# 桌面应用（应为 GUI）
DESKTOP_APPS = ["gallery", "gbcode", "louyue", "caiyun", "xiangqi", "cefu", "gedou"]
# 命令行工具（应保持 CONSOLE）
CLI_TARGETS = ["gedou_rl"]

SUBSYSTEM_GUI = 2
SUBSYSTEM_CONSOLE = 3


def binary_path(target: str) -> Path:
    suffix = ".exe" if sys.platform == "win32" else ""
    return ROOT / "build" / PROFILE / "bin" / f"{target}{suffix}"


def subsystem_of(path: Path) -> int:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path} 不是 PE 文件")
    optional = pe + 24          # PE 签名之后: COFF(20) + Optional 起点
    return struct.unpack_from("<H", data, optional + 68)[0]


def icon256_of(path: Path) -> str:
    """产物里 256×256 内嵌 PNG 的指纹。

    判据只用“变没变”，所以不要求与 `.ico` 里的字节完全相同：
    资源编译器会重编 PNG（分块/压缩级别可变），但**图像内容相同**即可。
    因此对前 200 字节取 md5——PNG 头 + IHDR + 首块 IDAT 开头已足以区分不同图标，
    且对同一图标的重复编译稳定。
    """
    data = path.read_bytes()
    index = data.find(b"\x89PNG\r\n\x1a\n")
    while index >= 0:
        width = int.from_bytes(data[index + 16:index + 20], "big")
        height = int.from_bytes(data[index + 20:index + 24], "big")
        if width == height == 256:
            return hashlib.md5(data[index:index + 200]).hexdigest()[:12]
        index = data.find(b"\x89PNG\r\n\x1a\n", index + 8)
    return "none"


def build(target: str, force: bool = False) -> tuple[int, str]:
    cmd = [str(ST), "build", target, "--profile", PROFILE]
    if force:
        cmd.append("--force")
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=1800)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def set_system(target: str, value: str) -> None:
    """把某目标里的 `"system": "..."` 改成指定值（只改该目标块内那一处）。"""
    text = PKG.read_text(encoding="utf-8")
    # 定位目标块：从 `"<name>": {` 到下一个同级 `}` 或下一个目标声明
    start = text.index(f'"{target}": {{')
    tail = text[start:]
    end = re.search(r'\n    (?="[a-z0-9_]+":\s*\{|\})', tail)
    block_end = start + (end.start() if end else len(tail))
    block = text[start:block_end]
    if '"system"' in block:
        new_block = re.sub(r'"system":\s*"[^"]*"', f'"system": "{value}"', block, count=1)
    else:
        new_block = block.rstrip()
        # 在块末尾的 `}` 之前插入
        close = new_block.rfind("}")
        new_block = new_block[:close] + f',\n                  "system": "{value}"\n' + new_block[close:]
    PKG.write_text(text[:start] + new_block + text[block_end:], encoding="utf-8")


def check_all() -> int:
    failures = 0
    for app in DESKTOP_APPS:
        binary = binary_path(app)
        if not binary.exists():
            print(f"  [SKIP] {app}: 产物不存在（先 st build {app}）")
            continue
        sub = subsystem_of(binary)
        icon = icon256_of(binary)
        ok_sub = sub == SUBSYSTEM_GUI
        ok_icon = icon != "none"
        mark = "OK  " if (ok_sub and ok_icon) else "FAIL"
        if not (ok_sub and ok_icon):
            failures += 1
        print(f"  [{mark}] {app:9} subsystem={sub} ({'GUI' if sub == 2 else 'CONSOLE'}) icon={icon}")

    for name in CLI_TARGETS:
        binary = binary_path(name)
        if not binary.exists():
            print(f"  [SKIP] {name}: 产物不存在")
            continue
        sub = subsystem_of(binary)
        ok = sub == SUBSYSTEM_CONSOLE
        if not ok:
            failures += 1
        print(f"  [{'OK  ' if ok else 'FAIL'}] {name:9} subsystem={sub} "
              f"({'CONSOLE（命令行工具应保持）' if sub == 3 else 'GUI —— 不该是'})")
    return failures


def reverse_verify() -> int:
    """逐项回退：把 system 改回 console / 换掉图标 → 构建 → 产物必须跟着变。

    这是唯一能证明“增量判定真的覆盖了这两项”的手段——
    只看构建输出（"完成"）是看不出来的。
    """
    failures = 0

    # ① system: gui → console，产物子系统必须变成 3
    target = "caiyun"
    binary = binary_path(target)
    if not binary.exists():
        print(f"  [SKIP] {target} 产物不存在")
        return 1
    original_pkg = PKG.read_text(encoding="utf-8")
    try:
        before = subsystem_of(binary)
        set_system(target, "console")
        build(target)
        after = subsystem_of(binary)
        ok = before == 2 and after == 3
        print(f"  [{'OK  ' if ok else 'FAIL'}] system gui→console: {before} → {after} (期望 2→3)")
        if not ok:
            failures += 1
    finally:
        PKG.write_text(original_pkg, encoding="utf-8")
        build(target)
    after = subsystem_of(binary)
    print(f"        恢复后 subsystem={after} (期望 2)")

    # ② 换图标：产物里 256 PNG 指纹必须变
    icon = ROOT / "examples" / "gedou" / "assets" / "gedou.ico"
    other = ROOT / "examples" / "louyue" / "assets" / "louyue.ico"
    gedou_bin = binary_path("gedou")
    if not (icon.exists() and other.exists() and gedou_bin.exists()):
        print("  [SKIP] 图标逆向验证所需文件不全")
        return failures
    original_icon = icon.read_bytes()
    try:
        before = icon256_of(gedou_bin)
        icon.write_bytes(other.read_bytes())
        build("gedou")
        after = icon256_of(gedou_bin)
        ok = before != after and after != "none"
        print(f"  [{'OK  ' if ok else 'FAIL'}] 换 .ico: {before} → {after} (期望不同)")
        if not ok:
            failures += 1
    finally:
        icon.write_bytes(original_icon)
        build("gedou")
    print(f"        恢复后 icon={icon256_of(gedou_bin)}")

    failures += check_all()
    return failures


def main() -> int:
    args = sys.argv[1:]
    if "--reverse" in args:
        failures = reverse_verify()
        print(f"\n{'逆向验证通过' if failures == 0 else f'{failures} 项失败'}")
        return 0 if failures == 0 else 1
    only = [a for a in args if not a.startswith("-")]
    if only:
        global DESKTOP_APPS
        DESKTOP_APPS = only
    print("桌面应用（应为 GUI + 有图标）：")
    failures = check_all()
    print(f"\n{'全部通过' if failures == 0 else f'{failures} 项失败'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
