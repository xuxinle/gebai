# third_party/ — 随仓库分发的第三方源码

这里的代码**已直接集成进仓库**：不是需要下载的依赖，也没有独立的版本更新流程。
构建时与框架自身源码一同编译，离线可构建、可审计。

| 依赖 | 版本 | 许可 | 用途 | 形态 |
|---|---|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | JSON 解析/序列化（替代原自研 st::json） | single-header（上游官方分发形态） |
| [quickjs-ng](https://github.com/quickjs-ng/quickjs) | 0.17.0 | MIT | 嵌入式 JS 引擎（应用内脚本层，可选） | 多文件 C 源码（仅保留引擎必需文件；已剔除 tests/CLI/wasm 与 quickjs-libc.c——后者提供 std/os 模块，本框架默认不给脚本系统访问能力） |
| [batterycenter/embed](https://github.com/batterycenter/embed) | 1.2.19 | Apache-2.0 | 编译期资源嵌入（b::embed<"path">()，含开发期热重载） | 上游为单 CMakeLists.txt；此处 vendor 其运行时与模板，构建期生成改由 stpm 原生实现（我们不依赖 CMake）——对照说明见 vendor/battery/UPSTREAM.md |
| [SQLite](https://sqlite.org/) | 3.50.2 | Public domain | 嵌入式数据库（`st::ext::Database`） | **amalgamation**（上游官方推荐的源码内置形态）：`sqlite3.c` + `sqlite3.h` + `sqlite3ext.h`；**编译期开关集中在 `sqlite/st_sqlite3_config.h`**（上游文件一字未改） |
| [three.js](https://threejs.org/) | 0.160.1 | MIT | 三维场景的**描述层**（`st::raster::ThreeRuntime`）：模型写 three.js，光栅化仍由本框架做 | CJS 产物 `three.cjs`（单文件，经 `b::embed` 编译期嵌入）；**不动上游一字**。选 0.160 而不是最新：模型的语料主力在 0.15x~0.16x，新版里 `BufferGeometry`/材质参数有改名 |
| [Microsoft WebView2](https://learn.microsoft.com/microsoft-edge/webview2/) | SDK 1.0.4314.35 | MIT（`LICENSE.txt`） | 可编程嵌入浏览器（`st::shell::webview` / `ui::WebView`）：URL 导航 + 资源内嵌（虚拟 origin）、执行 JS、双向消息 | **头文件原样**（`WebView2.h`，上游单文件分发形态）+ **运行时动态加载**的 `WebView2Loader.dll`（x64）。**只用头 + Loader，不含任何 Edge 二进制**；Loader 经 `b::embed` 编译期嵌入、首次使用时落地 |

## 合规要求（保留声明与许可）

- **Apache-2.0**（batterycenter/embed）：保留 `LICENSE` 与修改声明，见 `battery/UPSTREAM.md`
- **MIT**（nlohmann/json、quickjs-ng、three.js）：保留版权与许可文本，见各自目录 `LICENSE`
- **Public domain**（SQLite）：保留官方声明，见 `sqlite/SQLITE_PUBLIC_DOMAIN.txt`（无强制署名义务，但仍随源码附上）
- **MIT**（Microsoft WebView2 SDK）：保留许可与第三方声明，见 `webview2/LICENSE.txt` 与 `webview2/NOTICE.txt`（上游 NOTICE 含其内嵌组件的许可清单，随 SDK 一同分发）

## 逐文件校验和

代码已在仓库内，校验和的作用从防篡改转为**变更可追溯**：
改动第三方文件时应同步更新本表，让 review 一眼看出哪些上游代码被动了。

```bash
cd shuangtian/third_party && sha256sum -c CHECKSUMS.sha256
```

### 读到 `FAILED` 先看是不是行尾（**本仓的常态，不是被篡改**）

本仓库是 `core.autocrlf=true`：**索引里一律 LF**，Windows 工作区检出为 CRLF。
而校验和按**索引（LF）形态**计。所以在 Windows 工作区直接 `sha256sum -c` 时，
所有含换行的文本文件都会报 `FAILED`（实测本表 25 个文本条目全红，二进制条目全绿）。

判据（先归一化再比，而不是看那一行字）：

```bash
# 把工作区文件按 LF 归一化后再算，结果应全部 OK
python -c "import hashlib,sys;b=open(sys.argv[1],'rb').read();print(hashlib.sha256(b.replace(b'\r\n',b'\n')).hexdigest())" webview2/WebView2.h
```

所以本表的 SHA-256 一律是 **LF 形态**；新增条目必须按同一口径计，否则新克隆的仓
一校验就红（与“文件被动过”混淆，而那个结论要人命）。

| 文件 | 字节 | SHA-256（前 16 位） |
|---|---:|---|
| `nlohmann/json.hpp` | 953436 | `aaf127c04cb31c40...` |
| `quickjs/LICENSE` | 1212 | `96f73f9d2a16c21a...` |
| `quickjs/builtin-array-fromasync.h` | 5639 | `f41eb2eb028832cf...` |
| `quickjs/builtin-iterator-zip-keyed.h` | 15993 | `24446c6157a043cf...` |
| `quickjs/builtin-iterator-zip.h` | 16213 | `fdaef760ecce82c1...` |
| `quickjs/cutils.h` | 54740 | `dffec8bea17f0810...` |
| `quickjs/dtoa.c` | 44854 | `a8d2cf2d04db406e...` |
| `quickjs/dtoa.h` | 3357 | `1c4a4540c0038632...` |
| `quickjs/libregexp-opcode.h` | 2887 | `2a98d646089f3a72...` |
| `quickjs/libregexp.c` | 115889 | `af13e996abb1767f...` |
| `quickjs/libregexp.h` | 3612 | `53ff95a038f7001b...` |
| `quickjs/libunicode-table.h` | 251190 | `5840a921ac11664b...` |
| `quickjs/libunicode.c` | 61406 | `d755606498ff707d...` |
| `quickjs/libunicode.h` | 5278 | `ad13f66aaea3fab1...` |
| `quickjs/list.h` | 3177 | `cb6e24e8ee54bccd...` |
| `quickjs/quickjs-atom.h` | 8234 | `66654ae6533d1507...` |
| `quickjs/quickjs-c-atomics.h` | 2305 | `29883c742a5c5122...` |
| `quickjs/quickjs-opcode.h` | 15831 | `0054b0c45fd091af...` |
| `quickjs/quickjs.c` | 2148068 | `9fd0e0e68856d165...` |
| `quickjs/quickjs.h` | 66974 | `747a77444ff04a91...` |
| `battery/LICENSE` | 11357 | `c71d239df91726fc...` |
| `battery/UPSTREAM.md` | 2663 | `d05f8faa374e553f...` |
| `battery/embed.hpp.in` | 4207 | `fcb297b8ed28be5f...` |
| `battery/embed_impl.cpp` | 5684 | `7b8c9c44b8e1e44e...` |
| `battery/embed_source.cpp.in` | 1394 | `a26214317ce894c4...` |
| `sqlite/sqlite3.c` | 9281384 | `c9a0b6829b81d5f1...` |
| `sqlite/sqlite3.h` | 661946 | `7db44ac3e95c465c...` |
| `sqlite/sqlite3ext.h` | 38149 | `b184dd1586d93513...` |

## 来源与裁剪记录

### nlohmann/json 3.12.0

- 来源：`https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp`

### quickjs-ng 0.17.0

- 来源：`https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.17.0.tar.gz`
- 归档 SHA-256：`559bc4c420475e55c7ab4510adbc562f55d7524d75e8e89d79ce4bb02f5687d9`
- 已剔除： `qjs.c`, `qjsc.c`, `ctest.c`, `api-test.c`, `lre-test.c`, `qjs-wasi*.c`, `quickjs-libc.c`, `quickjs-libc.h`, `tests/`, `examples/`

### batterycenter/embed 1.2.19

- 来源：`https://github.com/batterycenter/embed/archive/refs/tags/v1.2.19.tar.gz`
- 已修改（附声明）： `embed.hpp.in`, `embed_source.cpp.in`, `embed_impl.cpp`

### SQLite 3.50.2

- 上游发布位：`https://sqlite.org/2025/sqlite-amalgamation-3500200.zip`
- 本次取数路径：crates.io 的 `libsqlite3-sys 0.35.0` 内 bundled 副本（`sqlite3/`）——
  该副本即由上述官方 zip 按文件 `unzip -p` 原样导出（见其 `upgrade.sh`），
  并以版本宏 + `SQLITE_SOURCE_ID` 交叉确认（均为 `3.50.2` / `2025-06-28 …`）。
  走这条路的原因：本机直连 sqlite.org 超时，而 SQLite 官方不授权镜像作权威分发。
- **未修改**：`sqlite3.c` / `sqlite3.h` / `sqlite3ext.h` 三件套保持上游原样。
- 本仓库新增（不改上游）：`sqlite/st_sqlite3_config.h`（编译期开关，经 `st.pkg` 的
  `c_flags` 以 `-include` 前置）、`sqlite/PROVENANCE.md`（来源与集成点）、
  `sqlite/SQLITE_PUBLIC_DOMAIN.txt`（上游声明誊录）。
- 复现与取舍记录：`docs/sqlite_integration.md`。

### Microsoft WebView2 SDK 1.0.4314.35

- 上游发布位：`https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4314.35/microsoft.web.webview2.1.0.4314.35.nupkg`
- 取出的文件（NuGet 包内路径 → 本仓库路径）：
  - `build/native/include/WebView2.h` → `webview2/WebView2.h`
  - `build/native/x64/WebView2Loader.dll` → `webview2/WebView2Loader.dll`
  - `LICENSE.txt` → `webview2/LICENSE.txt`；`NOTICE.txt` → `webview2/NOTICE.txt`
- **未修改**：上述四文件均保持上游原样（NuGet 包内容未作任何改写）。
- **刻意剔除**：
  - `build/native/x64/WebView2LoaderStatic.lib` 与 `WebView2Loader.dll.lib`：后者只能用于 MSVC 链接；
    前者是 MSVC 编译产物，实验确认不可用于 MinGW——报
    `multiple definition of IID_ICoreWebView2*`（静态库自带一份 `selectany` IID 定义）与
    `undefined reference to '__security_cookie' / '_Init_thread_epoch'`（MSVC 运行库符号）。
    ⇒ 本仓库只做**运行时 `LoadLibrary`**（这恰好也是 `CONVENTIONS.md` §10 第 1 条的要求）。
  - `lib/**`（.NET 程序集）、`runtimes/**`、`tools/wv2winrt/**`、`build/native/include-winrt/**`、
    `WebView2.idl`/`WebView2.tlb`：分别对应 .NET/WinRT 场景，C++ 直用 COM 口不需要。
  - `build/native/x86/`、`build/native/arm64/`：当前只支持 x64 目标。
- 与上游的**使用方式差异**（不是改文件）：`WebView2.h` 里的 `#pragma warning(...)`
  与 COM 接口的「有虚函数但无虚析构」在 GCC 下是 `-Werror` 级别告警，因此包含处必须用
  `#pragma GCC diagnostic ignored "-Wunknown-pragmas" / "-Wnon-virtual-dtor" / "-Wold-style-cast"`
  包一层（见 `src/shell/platform_win32_webview.cpp` 头部）。**不改上游头**是 §8 第三方纪律。

