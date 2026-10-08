#!/usr/bin/env python3
"""真窗口输入注入与量测库（Windows）。

## 为什么需要它

**控制通道的 `input.*` 与真窗口走的不是同一条路**：`input.key` 只发一条
`KeyDown`、`input.text` 只发一条 `TextInput`——而**真窗口上一次物理按键会产生
两条消息**（`WM_KEYDOWN` + `WM_CHAR`，应用的消息泵调了 `TranslateMessage`）。
涉及"一次按键"的契约（回显次数、字符落点、快捷键触发）**必须用真按键验证**，
否则测的是另一个输入形态。

固化本库的直接原因：2026-10-08 查"终端一字符回显两次"时，真窗口实测环节
因临时脚本反复返工（详见 `CONVENTIONS.md` §7.9），代价都在这些点上：

1. 每个按键起一个 PowerShell 进程 → **PowerShell 自己抢走前台焦点**，
   按键送不到目标窗口（表现为"完全没反应"，很容易误判成应用没处理）；
2. `keybd_event` 是发到**系统输入队列**的（走前台窗口），
   而后台/无焦点窗口收不到；`PostMessage` 又**不更新按键状态**，
   于是 `Ctrl+←` 这类修饰组合用 PostMessage 永远不生效；
3. 判读用"屏幕文本"会被 **PSReadLine 内联预测**（灰色幽灵文本，不是输入）
   和 **IME 组合窗**污染，读数不可信。

## 三条通道，按需要选（这是本库存在的核心）

| 通道 | 函数 | 会派发 `WM_CHAR`？ | 修饰键状态？ | 需要前台？ |
|---|---|---|---|---|
| **PostMessage** | `post_key` | ✅ 会（应用自己 `TranslateMessage`） | ❌ 不更新 | ❌ 不需要 |
| **keybd_event** | `press_key` | ✅ 会 | ✅ 真实 | ✅ 需要 |
| **SendInput** | `press_key(use_send_input=True)` | ✅ 会 | ✅ 真实 | ✅ 需要 |

- 验**无修饰**的按键（字符回显、`Enter`、方向键）→ 用 `post_key`：
  不需要抢前台，最稳，而且**只发 `WM_KEYDOWN` 一条**就够
  （再补一条 `WM_CHAR` 等于送两遍——这正是被测缺陷的原形，别把它写进测试夹具）。
- 验**带修饰**的组合（`Ctrl+←`、`Shift+Tab`）→ 用 `press_key`：
  修饰键的真实按下态是 `GetKeyState` 读的，PostMessage 不更新它。
  需要窗口在前台（`ensure_foreground`）。

## 量测：选不受重绘影响的量

- ✅ **光标列位移 / 元素属性面**（`get_property`）：不受重绘、无关画什么；
- ❌ **屏幕文本**：PSReadLine 会画**只有显示、不是输入**的内联预测文本，
  IME 组合窗也会叠字——2026-10-08 我因此把"预测文本"读成了"多回显的字符"。

用法（库）：见 `demo_probe()`；命令行：`python tools/st_win_input.py --help`。
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes  # noqa: N813  （Windows 类型别名）
import sys
import time

IS_WINDOWS = sys.platform == "win32"
if IS_WINDOWS:
    _user32 = ctypes.WinDLL("user32", use_last_error=True)
else:  # 允许在非 Windows 上 import（仅用于读文档/静态检查）
    _user32 = None

# —— 消息号 ——
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_CHAR = 0x0102
WM_SYSKEYDOWN = 0x0104

# —— 虚拟键码（只列常用的；其余直接传数字）——
VK = {
    "backspace": 0x08, "tab": 0x09, "enter": 0x0D, "shift": 0x10, "ctrl": 0x11,
    "alt": 0x12, "pause": 0x13, "capslock": 0x14, "esc": 0x1B,
    # 空格：注意 `" ".lower()` 仍是 `" "`，所以键名与字符两种写法都要收
    "space": 0x20, " ": 0x20,
    "pageup": 0x21, "pagedown": 0x22, "end": 0x23, "home": 0x24,
    "left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28,
    "insert": 0x2D, "delete": 0x2E,
}
VK.update({chr(c): c - 32 for c in range(ord("a"), ord("z") + 1)})   # 'a' -> 0x41
VK.update({str(d): 0x30 + d for d in range(10)})
# OEM 标点（美式布局的虚拟键码；这些键的 VK 与字符不同源，必须显式列）
VK.update({
    "`": 0xC0, "-": 0xBD, "=": 0xBB, "[": 0xDB, "]": 0xDD, "\\": 0xDC,
    ";": 0xBA, "'": 0xDE, ",": 0xBC, ".": 0xBE, "/": 0xBF,
})

# 需要 Shift 才能打出的字符 → (基础虚拟键, 用 Shift)。
# 为何需要：`PostMessage` 发的是**虚拟键**，而 `:` `?` `"` 这些字符与基础键
# 不是一一对应（`2`/`/`/`'`）——少了 Shift 状态就打不出来。
# 真键盘上这些字符本来也是「Shift + 基础键」，所以这里只是把物理事实写出来。
SHIFTED_CHARS = {
    "~": "`", "!": "1", "@": "2", "#": "3", "$": "4", "%": "5",
    "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
    "_": "-", "+": "=", "{": "[", "}": "]", "|": "\\",
    ":": ";", '"': "'", "<": ",", ">": ".", "?": "/",
}

KEYEVENTF_KEYUP = 0x0002


def vk_of(key) -> int:
    """键名 → 虚拟键码（`'a'` / `'Enter'` / `'enter'` / `0x41` 都能收）。"""
    if isinstance(key, int):
        return key
    text = str(key)
    if text in VK:
        return VK[text]
    lowered = text.lower()
    if lowered in VK:
        return VK[lowered]
    if lowered.startswith("0x"):
        return int(lowered, 16)
    raise KeyError(f"未知键名：{key!r}（可用：字母/数字/Enter/Tab/Esc/Left… 或直接给数字）")


# ════════════════════════════════════════════════════════════════════════════
# 窗口定位
# ════════════════════════════════════════════════════════════════════════════

def find_window(title_substring: str) -> int:
    """按标题**子串**找顶层窗口，返回 HWND（0 = 没找到）。

    注：标题可能重复（同进程多窗口、或多开），需要精确定位时请传具体 HWND。
    """
    if not IS_WINDOWS:
        raise RuntimeError("仅 Windows 可用")
    found: list[int] = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def _enum(hwnd, _lparam):
        length = _user32.GetWindowTextLengthW(hwnd)
        if length > 0:
            buffer = ctypes.create_unicode_buffer(length + 1)
            _user32.GetWindowTextW(hwnd, buffer, length + 1)
            if title_substring in buffer.value and _user32.IsWindowVisible(hwnd):
                found.append(hwnd)
        return True

    _user32.EnumWindows(_enum, 0)
    return found[0] if found else 0


def foreground_window() -> int:
    """当前前台窗口 HWND。"""
    return int(_user32.GetForegroundWindow())


def ensure_foreground(hwnd: int, retries: int = 8, wait_s: float = 0.08) -> bool:
    """把窗口激活到前台；返回是否成功。

    **`keybd_event` / `SendInput` 之前必须做这一步**：它们走系统输入队列，
    只有前台窗口收得到。失败时返回 `False`（调用方应改用 `post_key`，或先让用户点一下窗口）——
    Windows 有前台锁定，后台进程抢前台不一定成功，**不要无限重试**。
    """
    if foreground_window() == hwnd:
        return True
    for _ in range(retries):
        _user32.SetForegroundWindow(hwnd)
        time.sleep(wait_s)
        if foreground_window() == hwnd:
            return True
    return False


# ════════════════════════════════════════════════════════════════════════════
# 注入通道
# ════════════════════════════════════════════════════════════════════════════

def post_key(hwnd: int, key, with_keyup: bool = False) -> None:
    """`PostMessage` 发一条 `WM_KEYDOWN`（无修饰键的**首选**通道）。

    为什么**只发一条**就够：应用的消息泵会调 `TranslateMessage`，
    由它派生出对应的 `WM_CHAR`——这正是物理按键的真实形态。
    自己再补一条 `WM_CHAR` 就变成"一次按键两条文本事件"，
    会把**被测的缺陷原形**写进夹具（2026-10-08 踩过：补完后回显又变两遍）。

    局限：**不更新按键状态**（`GetKeyState(VK_CONTROL)` 读不到），
    因此带修饰的组合键请用 `press_key`。
    """
    _user32.PostMessageW(hwnd, WM_KEYDOWN, vk_of(key), 0x0001_0001)
    if with_keyup:
        _user32.PostMessageW(hwnd, WM_KEYUP, vk_of(key), 0xC000_0001)


def _keybd_event(vk: int, up: bool = False) -> None:
    _user32.keybd_event(vk, 0, KEYEVENTF_KEYUP if up else 0, 0)


def _send_input(vk: int, up: bool = False) -> None:
    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                    ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                    ("dwExtraInfo", ctypes.POINTER(wintypes.ULONG))]

    class _INPUTunion(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 24)]

    class INPUT(ctypes.Structure):
        _fields_ = [("type", wintypes.DWORD), ("u", _INPUTunion)]

    entry = INPUT(type=1)   # INPUT_KEYBOARD
    entry.u.ki = KEYBDINPUT(wVk=vk, wScan=0,
                            dwFlags=KEYEVENTF_KEYUP if up else 0, time=0, dwExtraInfo=None)
    _user32.SendInput(1, ctypes.byref(entry), ctypes.sizeof(INPUT))


def press_key(key, ctrl: bool = False, shift: bool = False, alt: bool = False,
              hold_ms: int = 30, *, hwnd: int | None = None, use_send_input: bool = False,
              require_foreground: bool = True) -> bool:
    """发**一次完整按键**（按下 + 抬起），带真实修饰键状态。

    返回是否发出（窗口不在前台且 `require_foreground` 时为 `False`）。

    何时用它而不是 `post_key`：**带修饰键的组合**——
    修饰键的真实按下态由 `GetKeyState` 读取，而 `PostMessage` 不更新按键状态，
    所以 `Ctrl+←` 用 `post_key` 永远送不出修饰语义（实测：只左移一格）。
    做法是先用 `keybd_event` 真按住修饰键，再发主键，最后抬起。

    ⚠ 走系统输入队列 ⇒ 目标窗口必须在前台（先 `ensure_foreground`）。
    """
    if require_foreground and hwnd is not None:
        if not ensure_foreground(hwnd):
            return False
    send = _send_input if use_send_input else _keybd_event
    modifiers = [VK["ctrl"] if ctrl else None, VK["shift"] if shift else None,
                 VK["alt"] if alt else None]
    held = [vk for vk in modifiers if vk is not None]
    try:
        for vk in held:
            send(vk, up=False)
        main = vk_of(key)
        send(main, up=False)
        time.sleep(hold_ms / 1000.0)
        send(main, up=True)
    finally:
        for vk in reversed(held):
            send(vk, up=True)
    return True


def type_text(hwnd: int, text: str, per_key_ms: int = 40, *, foreground: bool = False) -> int:
    """逐字符发真按键，返回成功发出的字符数。

    `foreground=False`（默认）走 `post_key`（**不抢前台**）；
    `foreground=True` 全部走 `press_key`。

    ⚠ **`PostMessage` 打不出需要 Shift 的字符**（大写字母、`:` `?` `"` `_` …）：
    `TranslateMessage` 派生 `WM_CHAR` 时读的是**全局按键状态**
    （`GetKeyState`），而 `PostMessage` 不更新它——把 Shift 位写进 lparam
    也没用（实测：发 `;` + Shift 位仍得到 `;`，不是 `:`）。
    这类字符**自动**改走 `press_key`（真按键，**需要前台**）。

    因此：纯小写/数字的输入用默认参数即可；**含大写或标点时窗口须在前台**，
    抢不到前台时停止并返回已发出的数量（调用方可据此判断少发了）。

    注意：这里**不能用剪贴板粘贴代替**——粘贴走 `WM_PASTE`，
    绕过了按键路径，验不到"按键 → 字符"这条契约。
    """
    sent = 0
    for ch in text:
        needs_shift = ch.isupper() or ch in SHIFTED_CHARS
        base = SHIFTED_CHARS.get(ch, ch.lower() if ch.isupper() else ch)
        if foreground or needs_shift:
            if not press_key(base, shift=needs_shift, hwnd=hwnd):
                break   # 抢不到前台：停下并如实报已发数量
        else:
            post_key(hwnd, base)
        sent += 1
        time.sleep(per_key_ms / 1000.0)
    return sent


def _post_with_shift(hwnd: int, base_key) -> None:
    """`PostMessage` 通道下发「Shift + 基础键」。

    `PostMessage` **不更新按键状态**，所以不能像 `keybd_event` 那样真按 Shift。
    能做的是**把 Shift 状态写进 lparam 的第 24 位**（扫描码高位）——那是
    `TranslateMessage` 用来派生正确 `WM_CHAR` 的依据。实测：不发这一位时
    `:` 会变成 `;`（Shift 丢失）。
    """
    shift_bit = 1 << 24
    _user32.PostMessageW(hwnd, WM_KEYDOWN, vk_of(base_key), (1 << 16) | 1 | shift_bit)


# ════════════════════════════════════════════════════════════════════════════
# 命令行（快速探针）
# ════════════════════════════════════════════════════════════════════════════

def demo_probe(title: str, text: str, foreground: bool) -> int:
    """最小探针：找窗口 → 打一串字符 → 报告前台状态。

    真项目里请**用控制通道读回属性面**（如终端光标列）来断言，
    不要用屏幕文本（会被 PSReadLine 预测文本 / IME 组合窗污染）。
    """
    hwnd = find_window(title)
    if hwnd == 0:
        print(f"找不到标题含 {title!r} 的可见窗口", file=sys.stderr)
        return 2
    print(f"窗口 HWND={hwnd}  前台={foreground_window() == hwnd}")
    if foreground:
        ok = ensure_foreground(hwnd)
        print(f"激活到前台: {'成功' if ok else '失败（前台锁定；请改 post_key 或先手动点窗口）'}")
        if not ok:
            return 3
    count = type_text(hwnd, text, foreground=foreground)
    print(f"已发出 {count} 个按键（通道：{'keybd_event' if foreground else 'PostMessage'}）")
    return 0


def main() -> int:
    if not IS_WINDOWS:
        print("本工具仅 Windows 可用（真窗口注入依赖 user32）", file=sys.stderr)
        return 1
    parser = argparse.ArgumentParser(description="真窗口按键注入探针（见文件头说明）")
    parser.add_argument("--title", default="歌白代码", help="窗口标题子串（默认 歌白代码）")
    parser.add_argument("--text", default="", help="要输入的字符（默认不输入，只报窗口）")
    parser.add_argument("--foreground", action="store_true",
                        help="走 keybd_event（需前台；修饰键/精确仿真用）")
    args = parser.parse_args()
    return demo_probe(args.title, args.text, args.foreground)


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    sys.exit(main())
