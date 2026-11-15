"""系统任务栏还原的**异常退出**验证：强杀之后任务栏会留在隐藏状态吗？

## 为什么要单独验这一条

`set_system_taskbar_visible(false)` 隐藏的是 **explorer 的** `Shell_TrayWnd`——
那是进程外的全局状态。后端在 `WM_DESTROY` 里兜底还原，应用用 RAII 在每条
**正常**退出路径还原；但 `TerminateProcess`（任务管理器"结束任务"、探针的
`proc.kill()`、崩溃）不会走 `WM_DESTROY`，也不执行任何 C++ 析构。

所以这条实验回答的是：**用户强杀桌面壳之后，桌面是不是就永久没有任务栏了？**
如果答案是"会"，那就是一个必须在文档里写明的使用约束（而不是"应该没事"）。

判据只看 `IsWindowVisible(Shell_TrayWnd)` 这一个量，且在**每次操作后**立刻读。
结尾无论结果如何都还原一次（不留副作用）。
"""

import ctypes
import subprocess
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
SW_SHOW, SW_HIDE = 5, 0


def taskbar_visible():
    tb = user32.FindWindowW("Shell_TrayWnd", None)
    if not tb:
        return None
    return bool(user32.IsWindowVisible(tb))


def restore():
    tb = user32.FindWindowW("Shell_TrayWnd", None)
    if tb:
        user32.ShowWindow(tb, SW_SHOW)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else r"build\dev\bin\st_desktop.exe"
    print(f"[probe] 初始任务栏可见 = {taskbar_visible()}")

    proc = subprocess.Popen([exe], cwd="build\\dev\\bin")
    try:
        time.sleep(3.0)
        hidden = taskbar_visible()
        print(f"[probe] 桌面壳运行中，任务栏可见 = {hidden}")
        if hidden is not False:
            print("[probe] !! 桌面壳没有隐藏任务栏，本实验前提不成立")
            return 1

        # 模拟"任务管理器结束任务"/崩溃：不留任何退出处理的机会
        proc.kill()
        proc.wait(timeout=10)
        time.sleep(1.2)
        after = taskbar_visible()
        print(f"[probe] 强杀之后，任务栏可见 = {after}")
        verdict = "已自动恢复（explorer 自己重画了）" if after else "仍然隐藏（需人工还原）"
        print(f"[probe] 结论：{verdict}")
        return 0 if after else 2
    finally:
        if proc.poll() is None:
            proc.kill()
        restore()
        time.sleep(0.5)
        print(f"[probe] 收尾还原后，任务栏可见 = {taskbar_visible()}")


if __name__ == "__main__":
    sys.exit(main())
