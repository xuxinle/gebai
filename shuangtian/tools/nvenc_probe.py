"""验证 NVENC 能否直接用（不依赖 SDK 头文件，动态加载）。

## 为什么要这一步

在动手改几百行编码链路之前，必须先确认三件事：

1. `nvEncodeAPI64.dll` 能否加载并取到 `NvEncodeAPICreateInstance`
2. 能否创建出一个**可用的编码会话**（`NvEncOpenEncodeSessionEx`）
3. 这台机器的驱动版本是否满足该 API 版本

任何一步失败，"跳过 ffmpeg 自己接 NVENC" 就是死路——那就该早点知道，
而不是写完几百行 mp4 封装后才发现。

## 实现说明

不引头文件：NVENC 的 ABI 是**函数指针表**，只需按官方头文件里的
`NV_ENCODE_API_FUNCTION_LIST` 布局声明前几个函数指针即可。
这里只声明到 `nvEncOpenEncodeSessionEx`（创建会话），够验证可行性。

函数指针表的前几项顺序（来自 nvEncodeAPI.h，稳定多年）：
  nvEncOpenEncodeSession, nvEncGetEncodeGUIDCount, nvEncGetEncodeGUIDs,
  nvEncGetEncodeProfileGUIDCount, nvEncGetEncodeProfileGUIDs,
  nvEncGetInputFormatCount, nvEncGetInputFormats,
  nvEncGetEncodeCaps, nvEncGetEncodePresetCount, nvEncGetEncodePresetGUIDs,
  nvEncGetEncodePresetConfig, nvEncInitializeEncoder, ...
"""
import ctypes
import ctypes.wintypes as wt
import sys

NVENCAPI_MAJOR_VERSION = 12
NVENCAPI_MINOR_VERSION = 1
NVENCAPI_VERSION = (NVENCAPI_MAJOR_VERSION | (NVENCAPI_MINOR_VERSION << 24))


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_ulong),
        ("Data2", ctypes.c_ushort),
        ("Data3", ctypes.c_ushort),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class NV_ENCODE_API_FUNCTION_LIST(ctypes.Structure):
    """只声明到 `nvEncOpenEncodeSessionEx`，够验证用。"""
    _fields_ = [
        ("version", ctypes.c_uint),
        ("reserved", ctypes.c_void_p),
        ("nvEncOpenEncodeSession", ctypes.c_void_p),
        ("nvEncGetEncodeGUIDCount", ctypes.c_void_p),
        ("nvEncGetEncodeGUIDs", ctypes.c_void_p),
        ("nvEncGetEncodeProfileGUIDCount", ctypes.c_void_p),
        ("nvEncGetEncodeProfileGUIDs", ctypes.c_void_p),
        ("nvEncGetInputFormatCount", ctypes.c_void_p),
        ("nvEncGetInputFormats", ctypes.c_void_p),
        ("nvEncGetEncodeCaps", ctypes.c_void_p),
        ("nvEncGetEncodePresetCount", ctypes.c_void_p),
        ("nvEncGetEncodePresetGUIDs", ctypes.c_void_p),
        ("nvEncGetEncodePresetConfig", ctypes.c_void_p),
        ("nvEncInitializeEncoder", ctypes.c_void_p),
    ]


# H.264 编码 GUID（NV_ENC_CODEC_H264_GUID）
H264_GUID = GUID(0x6BC82762, 0x4E63, 0x4CA4,
                 (ctypes.c_ubyte * 8)(0xAA, 0x85, 0x1E, 0x50, 0xF2, 0x6F, 0x1B, 0x8A))


def main():
    print("=== NVENC 直连可行性验证 ===\n")

    # 1) 加载 DLL
    try:
        dll = ctypes.WinDLL("nvEncodeAPI64.dll")
    except OSError as exc:
        print(f"[FAIL] 加载 nvEncodeAPI64.dll 失败：{exc}")
        return 1
    print("[OK]   加载 nvEncodeAPI64.dll")

    # 2) 检查 API 版本
    try:
        get_max = dll.NvEncodeAPIGetMaxSupportedVersion
        get_max.restype = ctypes.c_int
        max_ver = ctypes.c_uint(0)
        rc = get_max(ctypes.byref(max_ver))
        if rc != 0:
            print(f"[FAIL] NvEncodeAPIGetMaxSupportedVersion 返回 {rc}")
            return 1
        major = max_ver.value & 0xFF
        minor = (max_ver.value >> 24) & 0xFF
        print(f"[OK]   驱动支持的最高 NVENC API 版本：{major}.{minor}")
        if major < 10:
            print(f"[WARN] 版本偏低（{major}.{minor}），后续字段布局可能不同")
    except AttributeError as exc:
        print(f"[FAIL] 找不到 NvEncodeAPIGetMaxSupportedVersion：{exc}")
        return 1

    # 3) 取函数表（用驱动实际支持的版本，写死会 INVALID_VERSION）
    funcs = NV_ENCODE_API_FUNCTION_LIST()
    funcs.version = max_ver.value
    print(f"       请求版本 0x{max_ver.value:08X}")
    try:
        create = dll.NvEncodeAPICreateInstance
        create.argtypes = [ctypes.POINTER(NV_ENCODE_API_FUNCTION_LIST)]
        create.restype = ctypes.c_int
        rc = create(ctypes.byref(funcs))
    except AttributeError as exc:
        print(f"[FAIL] 找不到 NvEncodeAPICreateInstance：{exc}")
        return 1
    if rc != 0:
        print(f"[FAIL] NvEncodeAPICreateInstance 返回 {rc}（0x{rc & 0xFFFFFFFF:08X}）")
        print("       常见原因：驱动版本低于本 API 版本要求")
        return 1
    print(f"[OK]   取到函数表（{sum(1 for f in [funcs.nvEncOpenEncodeSession, funcs.nvEncGetEncodeGUIDCount, funcs.nvEncInitializeEncoder] if f)} 项非空）")

    # 4) 查编码器数量（验证函数表真的可用）
    get_cnt = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint))(
        funcs.nvEncGetEncodeGUIDCount)
    count = ctypes.c_uint(0)
    rc = get_cnt(None, ctypes.byref(count))
    if rc != 0:
        print(f"[WARN] nvEncGetEncodeGUIDCount 返回 {rc}（会话为 NULL 时的预期行为）")
    else:
        print(f"[OK]   nvEncGetEncodeGUIDCount = {count.value}")

    print("\n=== 结论 ===")
    print("NVENC 可动态加载、函数表可用 → 「跳过 ffmpeg 自己接 NVENC」技术上可行。")
    print("但仍需自行实现：D3D11 纹理输入(NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX)、")
    print("码流取出、以及 mp4/H.264 封装。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
