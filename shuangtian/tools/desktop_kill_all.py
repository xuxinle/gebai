"""停掉所有 st_desktop 实例（含任务栏守卫），并确保系统任务栏回到可见。

## 为什么不能直接 `Stop-Process -Name st_desktop -Force`

桌面壳隐蒕系统任务栏时，会拉起一个**同 exe 的守卫子进程**（`--taskbar-guard`）
负责“父进程一死就把任务栏显回来”。按**映像名批量杀**会把守卫一起杀掉——
它没机会还原，用户的桌面就永久没有任务栏（实测踩过）。

本脚本按**命令行**区分主进程与守卫：
1. 先杀主进程（守卫仍在 → 它会自己还原任务栏并退出）；
2. 等 2.5s；
3. 仍有守卫残留才强杀它，并手动还原任务栏。
"""

import ctypes
import subprocess
import time
import sys

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


def processes():
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
        return []
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
    found = []
    ok = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower() == "st_desktop.exe":
            found.append((entry.th32ProcessID, entry.th32ParentProcessID))
        ok = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    kernel32.CloseHandle(snapshot)
    return found


def command_line(pid):
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             f"(Get-CimInstance Win32_Process -Filter 'ProcessId={pid}').CommandLine"],
            capture_output=True, text=True, timeout=15).stdout
        return out.strip()
    except Exception:
        return ""


def taskbar_visible():
    tb = user32.FindWindowW("Shell_TrayWnd", None)
    return bool(user32.IsWindowVisible(tb)) if tb else None


def main():
    found = processes()
    if not found:
        print("没有 st_desktop 在跑")
        return 0

    main_pids, guard_pids = [], []
    for pid, _parent in found:
        line = command_line(pid)
        (guard_pids if "--taskbar-guard" in line else main_pids).append(pid)
    print(f"主进程 {main_pids} · 守卫 {guard_pids}")

    # ① 先杀主进程，让守卫自己收尾（它会还原任务栏后退出）
    for pid in main_pids:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    if main_pids:
        time.sleep(2.5)

    # ② 守卫残留才动手
    left = [pid for pid, _ in processes()]
    if left:
        print(f"守卫残留 {left}（强杀 + 手动还原任务栏）")
        for pid in left:
            subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
        handle = user32.FindWindowW("Shell_TrayWnd", None)
        if handle:
            user32.ShowWindow(handle, 5)
        time.sleep(0.6)

    remaining = [pid for pid, _ in processes()]
    print(f"剩余进程: {remaining}")
    print(f"任务栏可见: {taskbar_visible()}")
    return 0 if not remaining else 2


if __name__ == "__main__":
    sys.exit(main())
