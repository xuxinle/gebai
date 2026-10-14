# third_party/wayland/ — 协议 XML 台账

与 `third_party/` 的其它内联依赖同一口径（见 `../SOURCES.md`）：**代码进仓库**，
因此每份外部输入都要说清"从哪来、是哪一版"。协议 XML 是**规范本身**（不是某实现的代码），
这里记录的是官方仓库的原始副本。

校验：`cd third_party/wayland && sha256sum -c CHECKSUMS.sha256`

| 文件 | 来源 | SHA-256 |
|---|---|---|
| `wayland.xml` | `gitlab.freedesktop.org/wayland/wayland/-/raw/main/protocol/wayland.xml` | `4831fd1c…dac294` |
| `xdg-shell.xml` | `gitlab.freedesktop.org/wayland/wayland-protocols/-/raw/main/stable/xdg-shell/xdg-shell.xml` | `8cd3980d…47d9de` |
| `xdg-decoration-unstable-v1.xml` | `gitlab.freedesktop.org/wayland/wayland-protocols/-/raw/main/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml` | `68753c4a…fbecf2f` |

## 各文件承担什么

| 文件 | 内容 | 后端用它做什么 |
|---|---|---|
| `wayland.xml` | 核心协议：`wl_display`/`wl_registry`/`wl_compositor`/`wl_surface`/`wl_shm`/`wl_shm_pool`/`wl_buffer`/`wl_callback`/`wl_seat`/`wl_keyboard`/`wl_pointer`/`wl_output`/`wl_region` 等 23 个接口 | 连显示、建 surface、共享内存送帧、收发输入 |
| `xdg-shell.xml` | 桌面外壳（stable）：`xdg_wm_base`/`xdg_positioner`/`xdg_surface`/`xdg_toplevel`/`xdg_popup` | 窗口的创建、configure 握手、标题、移动/缩放请求、最大化 |
| `xdg-decoration-unstable-v1.xml` | 去装饰：`zxdg_decoration_manager_v1`/`zxdg_toplevel_decoration_v1` | **自绘窗框的前提**：`xdg-shell` 本身没有"不要服务端装饰"的表示，而 GNOME 默认给 `xdg_toplevel` 加服务端标题栏，必须显式声明 client-side（`CONVENTIONS.md` §10 第 7 条） |

许可：三者均来自 freedesktop.org 官方仓库，文件头部自带 MIT 许可声明（见各文件版权段）。

## 为什么不用 `wayland-scanner`

目标机器（含本机）常常**既没有 `wayland-scanner`，也没有 `libwayland-dev` 头文件**，
更没有 `/usr/share/wayland-protocols` 目录。用 `tools/wayland_bindgen.py` 自己生成
接口表（消息签名 + 类型表 + opcode），才能保持"**零编译期依赖、运行时 `dlopen`**"
（`CONVENTIONS.md` §10 第 1 条）——与 X11 后端同一姿态。

生成物 `src/shell/wayland_protocol.hpp` **入库**，构建不联网、不依赖上述任何工具。
重新生成：

```bash
python3 tools/wayland_bindgen.py --out src/shell/wayland_protocol.hpp
```

## 参考副本（**不参与构建**，只作 ABI 依据留档）

| 文件 | 来源 | 用途 |
|---|---|---|
| `wayland-util.h` | `wayland/-/raw/main/src/wayland-util.h` | `wl_message`/`wl_interface` 的字段顺序与语义（我们照它构造 `wayland_protocol.hpp` 里的同名结构） |
| `connection.c` | `wayland/-/raw/main/src/connection.c` | `wl_closure_marshal`（参数按签名逐项取）、`convert_arguments_to_ffi`（**事件回调的确切签名**：`array` 传数组指针、`fd` 传 `int32_t`、`new_id` 客户端侧传对象指针）、`_wl_closure_vmarshal`（`va_arg` 提升规则） |

这两份文件存在的唯一目的是**让后端的 ABI 假设可被复查**：缺头文件的环境里，
"事件回调该写什么签名""`new_id` 参数传什么"这类问题必须有据可查，而不是凭记忆。
它们放在 `refs/` 子目录（**不参与构建**：`st.pkg` 的 `third_party_sources` 只收
`third_party/quickjs/*.c` 与 `third_party/sqlite/*.c`，本目录不在其中）。
