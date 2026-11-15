"""守卫进程生命期检查：正常/优雅退出后，守卫是否自行退出、任务栏是否还原。

## 为什么要单独测这一条

隐蒕系统任务栏时，桌面壳会拉起一个同 exe 的守卫子进程（`--taskbar-guard`）。
它存在的意义是**强杀也能还原**（见 `desktop_taskbar_kill_probe.py`），
但反过来引入两个必须验的风险：

1. **守卫会不会残留**？ 若正常退出后它还在跑，用户会在任务管理器里看到
   两个同名进程，而双击 exe 又会多一个——「桌面开好几份」。
2. **正常退出路径下任务栏是否还还原**？ 能还原但留下僵尸守卫仍然不可接受。

判据：计数 `st_desktop.exe` 进程数 + `IsWindowVisible(Shell_TrayWnd)`，
三个时刻各读一次：运行中 / 退出后立刻 / 退出后 2 秒。
"""

import ctypes
import subprocess
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


def taskbar_visible():
    tb = user32.FindWindowW("Shell_TrayWnd", None)
    if not tb:
        return None
    return bool(user32.IsWindowVisible(tb))


def count_processes(name="st_desktop.exe"):
    """按映像名数进程（不依赖 PowerShell 的引号嵌套）。"""
    TH32CS_SNAPPROCESS = 0x00000002

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", ctypes.c_uint32), ("cntUsage", ctypes.c_uint32),
            ("th32ProcessID", ctypes.c_uint32), ("th32DefaultHeapID", ctypes.c_void_p),
            ("th32ModuleID", ctypes.c_uint32), ("cntThreads", ctypes.c_uint32),
            ("th32ParentProcessID", ctypes.c_uint32), ("pcPriClassBase", ctypes.c_long),
            ("dwFlags", ctypes.c_uint32), ("szExeFile", ctypes.c_wchar * 260),
        ]

    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snapshot == -1:
        return -1
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
    count = 0
    ok = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower() == name.lower():
            count += 1
        ok = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    kernel32.CloseHandle(snapshot)
    return count


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else r"build\dev\bin\st_desktop.exe"
    baseline = count_processes()
    print(f"[probe] 启动前：进程数={baseline} 任务栏={taskbar_visible()}")
    if baseline:
        print("[probe] 已有实例在跑，实验前提不成立（请先清理）")
        return 1

    proc = subprocess.Popen([exe], cwd="build\\dev\\bin")
    try:
        time.sleep(3.0)
        running = count_processes()
        print(f"[probe] 运行中：进程数={running}（含守卫）任务栏={taskbar_visible()}")
        ok = True
        if running != 2:
            print(f"[probe] !! 期望 2 个（主+守卫），实际 {running}")
            ok = False
        if taskbar_visible() is not False:
            print("[probe] !! 任务栏未隐藏")
            ok = False

        # 优雅退出（SIGTERM 等价于关闭窗口请求）
        proc.terminate()
        proc.wait(timeout=10)
        print(f"[probe] 退出后 0s：进程数={count_processes()} 任务栏={taskbar_visible()}")
        time.sleep(2.0)
        left = count_processes()
        bar = taskbar_visible()
        print(f"[probe] 退出后 2s：进程数={left} 任务栏={bar}")
        if left != 0:
            print(f"[probe] !! 守卫残留（{left} 个进程）")
            ok = False
        if bar is not True:
            print("[probe] !! 任务栏未还原")
            ok = False
        print(f"[probe] 结论：{'通过' if ok else '失败'}")
        return 0 if ok else 2
    finally:
        if proc.poll() is None:
            proc.kill()
        tb = user32.FindWindowW("Shell_TrayWnd", None)
        if tb:
            user32.ShowWindow(tb, 5)


if __name__ == "__main__":
    sys.exit(main())
