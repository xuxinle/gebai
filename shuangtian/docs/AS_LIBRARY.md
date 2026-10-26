# 把霜天当通用 C++ 库用

> 本文回答三件事：**哪些部分能不碰界面直接用**、**怎么最小引入**、**有什么代价与局限**。
> 所有数字来自 2026-10-26 在本机（Windows 11 / MinGW-w64 GCC 16.2 / 28 逻辑核）的实测，
> 复现命令在 §6。

## 1. 定位

霜天是一个**自绘、跨平台的原生应用框架**——它的主体是窗口、组件、绘制、字体、文本、输入。
但也正因为「从零自建」，它有相当一部分是**与界面无关的基础设施**：
文件系统、错误模型、字符串、时间、JSON、PNG 编解码、TCP、XML、LSP 协议、包管理。

这部分可以用在任何 C++ 程序里，包括**完全没有任何界面的命令行工具与服务**。

判定标准不是"我觉得它通用"，而是**依赖闭包的实测结果**：一个模块（含它所有头文件）
在传递闭包里**不触及 `st/ui/` 与 `st/raster/`**，它就能独立成库。见 §2。

## 2. 能直接用哪些（实测）

`st_core` 库目标（`st.pkg` 里的 `"kind": "library"`）当前包含 **9 个模块、66 个对象、35 MB**：

| 模块 | 头文件 | 头行数 | 实现行数 | 内容 |
|---|---|---|---|---|
| **core** | 27 | 2,470 | 6,055 | 错误模型 `Result<T>`、文件系统、字符串、时间、日志、控制台镜像、进程、线程池、哈希、base64、内存映射 |
| **pkg** | 13 | 1,333 | 8,053 | 自研包管理器：清单解析、语义版本、构建驱动、lint、注册表、stats |
| **lsp** | 6 | 809 | 1,762 | Language Server Protocol 客户端与类型（protocol / client / completion / navigation / edit / actions）|
| **net** | 3 | 390 | 1,480 | TCP（含跨平台 socket 封装）|
| **codec** | 3 | 166 | 1,791 | PNG 编解码、DEFLATE、归档 |
| **math** | 4 | 672 | 677 | 几何（点/矩形/变换）、颜色、**矢量路径**（`Path`：命令流 + 贝塞尔扁平化 + 包围盒）|
| **test** | 1 | 365 | 536 | 单元测试框架（`ST_TEST` / `ST_CHECK` / 分片 / JUnit 输出）|
| **xml** | 1 | 114 | 469 | XML 解析 |
| **edit** | 1 | 202 | 112 | 文本编辑的纯数据模型（无界面）|
| **合计** | **59** | **6,521** | **20,935** | 公开 API 约 **432** 处 |

### 不能直接用的（依赖界面）

| 模块 | 为什么 |
|---|---|
| `ui` / `raster` / `shell` / `app` | 就是界面本身 |
| `text` | 字的**度量与栅格化**要 `raster::Canvas` / `raster::Surface`（字形位图 + 上屏）|
| `md` | 渲染要 `text` |
| `ext` | `script`（QuickJS 脚本层）依赖 `ui`；注意 `ext/json` 已归位到 `core` |
| `control` | 控制通道要操作 `ui::actions` / `ui::dsl` 的具体元素 |
| `probe` | 探针外壳本身无界面依赖，但它只服务于本仓的工具 |

## 3. 怎么最小引入

### 3.1 在你的 `st.pkg` 里引用框架

独立工程不需要安装框架，也不需要 CMake——清单里写一句 `framework.path` 即可，
框架的源/头/标志/嵌入/交叉编译工具链会自动并入：

```json
{
  "shuangtian_pkg_format": 1,
  "name": "mytool",
  "version": "0.1.0",
  "cxx_standard": 23,
  "framework": { "path": "../shuangtian" },
  "sources": ["src/*.cpp"],
  "targets": {
    "mytool": {
      "kind": "executable",
      "libraries": ["st_core"],
      "sources": ["src/*.cpp"]
    }
  }
}
```

`"libraries": ["st_core"]` 会让构建器**先自动构建库，再链接它**（不必手动分两步）。

### 3.2 只想要「零构建系统依赖」？

如果你不想引入 `st` 工具链，`st_core` 就是个普通静态库——
用你自己的 CMake/Make 直接链 `build/dev/bin/st_core.a`，头文件目录是 `include/`：

```cmake
# 实测过的完整链接行（MinGW-w64 GCC 16.2 / Windows 11）
target_include_directories(mytool PRIVATE ${SHUANGTIAN}/include ${SHUANGTIAN}/third_party)
target_link_libraries(mytool PRIVATE ${SHUANGTIAN}/build/dev/bin/st_core.a
                      ws2_32 user32 gdi32 shell32 dbghelp)
```

**这几个库不是“可能用到”，是必需**（2026-10-27 实测：缺一个就链接失败）：

| 库 | 谁要它 |
|---|---|
| `ws2_32` | `net` 模块的 socket |
| `dbghelp` | `core/platform_crash.cpp` 的崩溃栈回溯（`SymInitialize` / `StackWalk64`）|
| `user32` / `gdi32` / `shell32` | `core` 的进程/控制台与嵌入资源运行时 |

另外需要 `third_party` 在包含路径里（`json.hpp` 要引 `nlohmann/json.hpp`）。

### 3.3 一个非 GUI 的最小例子

```cpp
// 用 core 的错误模型 + 文件系统 + JSON（不需要任何界面）
#include "st/core/entry.hpp"    // ST_MAIN：跨平台入口（Windows 的 argv 是 ANSI，中文参数会乱）
#include "st/core/error.hpp"
#include "st/core/fs.hpp"
#include "st/core/json.hpp"

auto load_config(const std::string& path) -> st::Result<int> {
  auto text = st::fs::read_text(path);
  if (!text) return st::forward_error(text.error());   // 错误链原样上抛

  auto doc = st::json_parse(*text);                    // 解析失败带“第 N 字节”诊断
  if (!doc) return st::forward_error(doc.error());

  // 读取用 json_get_*，**不要**写 (*doc)["port"]：
  // nlohmann 的 const operator[] 在键不存在时是未定义行为。
  return static_cast<int>(st::json_get_i64(*doc, "port", 8080));
}

auto run(int /*argc*/, char** /*argv*/) -> int {
  if (auto port = load_config("config.json"); port) {
    std::printf("port = %d\n", *port);
    return 0;
  } else {
    std::fprintf(stderr, "%s\n", port.error().message.c_str());
    return 1;
  }
}

ST_MAIN(run)   // 展开成跨平台 main，内部做控制台编码/诊断/崩溃处理器初始化
```

上面这段是**实测编译并运行过的**（缺键退默认值、坏 JSON 报错、文件缺失报错，三种都验过）。

这三个东西值得单独说明：

- **`st::Result<T>`**：`std::expected<T, Error>` 的薄门面，业务错误显式传播、不用异常。
- **`st::fs`**：跨平台文件系统，路径一律 UTF-8。**Windows 上这点很关键**——
  `std::filesystem::path(std::string)` 会按 ANSI 代码页解释，中文路径直接找不到文件。
- **`st::Json`**：**基于 nlohmann/json v3.12.0**（内联在 `third_party/`），
  外面包一层薄封装把它的**抛异常接口收敛成 `Result`**（`json_parse` → `Result`，
  `json_get_*` 类型不符退默认值不抛）。选用 nlohmann 而非自研的原因：
  旧自研实现把数字统一存 `double`，雪花 ID `1234567890123456789` 会变
  `1.2345678901234568e+18`（详见 `CONVENTIONS.md` §3.7）。

## 4. 为什么这些能独立：分层规则

模块分层的**唯一硬约束**是：**下层不得依赖上层**。当前实测（`include/` + `src/` 全量扫描）：

- 47 条跨模块依赖边
- **8 处**「下层依赖上层」的违规（原 17 处 → 10 处（json/async 归位 core）
  → 8 处（Path 下沉 math）；逐条论证见 `STRUCTURE.md`）

违规集中在三处，都是**真实的跨层需求**而非分类错误：

| 违规 | 处数 | 性质 |
|---|---|---|
| `control → ui` | 5 | 控制通道本就要操作 UI 元素 |
| `text → raster` | 4 | 字形轮廓用 `raster::Path` 表示 |
| `md → text` | 1 | 语法高亮要用 text 的类型 |

这就是 `text` / `control` / `md` **进不了 `st_core`** 的原因——不是它们"不够通用"，
而是它们的功能本身就横跨了界面层。

## 5. 局限（诚实说明）

1. **`st_core` 不是"小"库**：35 MB 静态库、6,408 行头、425 处公开 API。
   如果你只需要 JSON，拿 `st/core/json.hpp`（99 行头 + 254 行实现）就够了，不必链整个库。
2. **C++23**：框架用 `std::expected` / `std::mdspan` / `std::flat_map`，要求编译器支持 C++23。
   实测可用：GCC 16.2、Clang 19+。MSVC 未验证。
3. **`pkg` 模块（8,053 行）是自研包管理器**，一般项目用不到，但它与 `core` 同在一层，
   链 `st_core` 会一起带进来。要瘦身得单独切一个更小的库目标。
4. **没有 ABI 稳定性承诺**：框架与使用方按源码级依赖同版本构建（`framework.path`），
   不做二进制兼容。
5. **`text` 模块的教训**：它是最典型的"看起来该通用、实际不通用"——
   字体度量需要真实的字形轮廓几何，而那个几何在 `raster` 里。

## 6. 复现本文的数字

```powershell
# ① 库的构成
st build st_core --profile dev
ar t build/dev/bin/st_core.a | Measure-Object -Line          # 65 个成员
Get-Item build/dev/bin/st_core.a                             # 35 MB

# ② 哪些模块在库白名单里
Select-String -Path st.pkg -Pattern '"st_core"' -Context 0,6

# ③ 分层违规（需自备扫描脚本；判据：下层 include 上层）
#    当前基线：10 处
```
