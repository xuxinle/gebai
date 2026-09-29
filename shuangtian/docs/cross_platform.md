# 10. 跨平台强制约束（写代码时必须逐条自查）

框架的目标平台是 **Linux / Windows / macOS**，且必须支持**交叉编译**（如 Linux 上产出 Windows 程序）。
跨平台不是"以后再说"的收尾工作：**一处平台假设会让整个目标平台编不过或运行期出错**，
而它在本机（Linux）往往完全看不出来。因此这些是**强制约束**，与 §2 的禁令同等地位。

## 10.1 平台差异只能出现在 `platform_*` 里

| 允许 | 位置 |
|---|---|
| 系统头（`<windows.h>`、`<unistd.h>`、`<sys/socket.h>`、`<dlfcn.h>`…） | `src/**/platform_*.cpp` |
| 平台宏（`_WIN32`/`__linux__`/`__APPLE__`） | 同上；**极少数**确需在公共代码分叉时，必须两侧都能编译并有注释说明 |
| 平台的 `char*` API 边界（`execvp`、`CreateProcess`、宽字符转换） | 同上（§2 R6 的受控例外） |

**跨平台代码不得直接**：`#include <unistd.h>`、调 `getpid()`/`fork()`/`dlopen()`、用 `windows.h` 类型。
需要这类能力时**先在后端加一个平台无关的封装**（如 `process::current_id()`），再在平台文件里实现两侧。

> 反例（真实缺陷）：控制通道 `hello` 直接调 `::getpid()` → Linux 编得过，**Windows 交叉编译直接报
> `'::getpid' has not been declared`**；而且那里的写法 `getpid() == 0 ? 0 : 0` 恒为 0，pid 从来没上报对过。
> 正解：`process::current_id()`，Windows 侧走 `GetCurrentProcessId()`。

## 10.2 路径：一律 UTF-8 文本，进出都经 `st::fs`

- 框架对内对外**统一 UTF-8** `std::string` 表示路径（与源码、JSON、脚本一致）。
- 路径拼接/比较/绝对路径判定**必须用 `st::fs`**（`join`/`relative_to`/`is_absolute`），
  不要手写 `base + "/" + leaf`（Windows 分隔符是 `\`，且存在盘符与 UNC 路径）。
- **文件流不要用 `std::string` 直接构造**：Windows 上 `std::ifstream(std::string)`
  按**本地 ANSI 代码页**解释，中文路径必然打不开。
  `fs` 层已用宽字符路径（`std::filesystem::path` 在 Windows 内部是 `wstring`）——
  新代码走 `fs::read_text/read_bytes/write_text`，或 `fs::to_path()`/`fs::to_utf8()`。
- 大小写：Windows/macOS 文件系统**不区分大小写**，别用大小写来区分文件。

```cpp
// 反例：中文路径在 Windows 上必然失败
std::ifstream stream(std::string(path));            // ✗
// 正例：走 fs 层（内部已是宽字符路径）
auto text = st::fs::read_text(path);                // ✓
// 正例：需要自己拿 path 时
auto stream = std::ifstream(st::fs::to_path(path)); // ✓
```

## 10.3 进程入口：`argv` 在 Windows 是 ANSI

命令行参数在 Windows 是**本地 ANSI 编码**（中文机器是 GBK），Linux/macOS 才是 UTF-8。
示例与工具程序统一用 `ST_MAIN(fn)`（`st/core/entry.hpp`）：它在入口处把参数正规化成 UTF-8，
顺带把 Windows 控制台代码页设为 UTF-8（否则中文日志乱码）。

```cpp
#include "st/core/entry.hpp"
auto run_app(int argc, char** argv) -> int { /* 这里一定是 UTF-8 */ }
ST_MAIN(run_app)
```

## 10.4 类型与格式化

| 陷阱 | 规则 |
|---|---|
| `long` 在 Windows 是 32 位、Linux 64 位 | 需要定宽就用 `std::int32_t/int64_t/std::size_t`，**不要用 `long` 表示字节数/偏移** |
| 打印 | 一律 `std::format`/`st::print`（避免 `%ld`/`%lld` 的平台差异；L7 已全局禁 printf） |
| 字符类型 | 文本一律 `char` + UTF-8；Windows 宽字符只在平台文件的 API 边界出现 |
| 结构体布局/打包 | 不假设 ABI；需要固定布局用显式类型与 `static_assert` |
| `size_t` 与整数混算 | 保持 `-Wconversion` 干净（本项目已开 `-Werror`，跨平台后更值得保持） |

## 10.5 系统库、工具链与构建

- **系统库按目标平台解析**（`default_system_libs(platform)`），**不能**用宿主宏判断：

  | 平台 | 系统库 |
  |---|---|
  | linux | `pthread` `dl` `m` |
  | windows | `ws2_32`（交叉工具链可声明 `winpthread` 等） |
  | darwin | （默认无；按需声明） |

- 交叉编译在 `st.pkg` 的 `toolchains` 段声明（编译器、目标平台、系统库、宏、产物后缀），
  用 `st build <target> --toolchain=<名>` 启用；**产物与中间目录隔离**为 `build/<档位>-<工具链>/`。
- 交叉编译时**不套用宿主的可选特性**（如 `-fuse-ld=lld`/`mold`：宿主装的不一定支持目标格式）。
- 交叉产物的**运行时行为**无法在本机验证，因此新增平台分支必须至少做到"交叉编译通过"。

## 10.6 自查清单（提交前逐条过）

1. 有没有新增系统头 / 平台宏 / `char*` API？→ 必须在 `platform_*` 里。
2. 有没有直接构造文件流或用字符串拼路径？→ 换成 `st::fs`。
3. 有没有假设 `long` 是 64 位、或依赖整数宽度？（本机跑不出问题）
4. 有没有在非平台文件里用 POSIX 函数（`getpid`/`usleep`/…）？
5. Windows 分支改了？→ 跑 `st build <target> --toolchain=mingw` 确认**编得过**
   （本机编不到 Windows 分支，这是唯一能发现问题的途径）。
6. 新增系统库依赖？→ 在 `toolchains` 里为目标平台声明。
