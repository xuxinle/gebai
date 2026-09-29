# 独立工程使用霜天（框架 + 工具链）

面向"在**别处**写一个霜天应用"：不需要把代码放进框架仓库，不需要安装，不需要 CMake，
一条清单字段即完成引用。

## 最快路径

```bash
# 1. 生成工程（用框架自带的 st；也可用 --framework=<框架根> 从任意位置运行）
/path/to/shuangtian/build/bin/st init ./myapp --name myapp

# 2. 写代码
$EDITOR myapp/src/main.cpp

# 3. 构建 / 运行
cd myapp && ./build.sh                 # 等价于 <框架>/build/bin/st build myapp
./build/debug/bin/myapp --headless --control-port 0 --control-file /tmp/app.json

# 4. 交叉编译出 Windows 可执行（工具链继承自框架，工程无需声明）
./build.sh --profile release --toolchain=mingw
# → build/release-mingw/bin/myapp.exe
```

## 清单长什么样

```jsonc
{
  "name": "myapp",
  "version": "0.1.0",
  "kind": "executable",
  "framework": { "path": "/path/to/shuangtian" },   // 相对路径按清单目录解析
  "sources": [],                                     // 顶层源留空，避免与 target 重复匹配
  "targets": {
    "myapp": { "kind": "executable", "sources": ["src/*.cpp"],
               "embed": ["assets/*"] }               // 可选：编译期嵌入的资源
  }
}
```

`framework` 也接受字符串简写：

```jsonc
"framework": "../shuangtian"
```

可选字段 `inherit_flags: false`：不继承框架的严格告警集（默认继承——**推荐继承**，
否则会出现"框架里干净、工程里漏报"）。

## 引用框架时自动并入什么

| 并入 | 说明 |
|---|---|
| 框架源 | `src/**/*.cpp`（**排除 `src/pkg/*`**：那是工具链自身，应用用不到） |
| 包含目录 | 框架的 `include` 与 `third_party` |
| 编译标志与宏 | 框架的严格告警集、`-fno-strict-aliasing`、`ST_VERSION` 等（可用 `inherit_flags: false` 关掉） |
| 第三方 C 源 | QuickJS 等（按第三方对待：放宽告警、不进 PCH、不做 sanitizer 插桩） |
| 编译期嵌入 | 框架的 `src/ui/script_api.js`（脚本能力需要），生成物落在**框架目录**下 |
| 系统库 | `pthread/dl/m`（Linux）、`ws2_32`/`gdi32`/`user32`（Windows 目标） |
| 交叉编译工具链 | 框架的 `toolchains` 段（工程同名可覆盖） |

**不并入**：框架的 `targets`、`tests`、`src/pkg/*`。工程不跑框架的单测，也不需要工具链实现。

## 嵌入资源（`b::embed<"..."> ()`）

```cpp
#include "battery/embed.hpp"     // 框架的生成物，已在包含路径上
const auto logo = b::embed<"assets/logo.png">();   // 路径写错**编译期**报错
logo.data(); logo.length(); logo.str();
```

清单里声明 glob 即可（上例的 `"embed": ["assets/*"]`）。两种作用域：

- **目标级**（`targets.<名>.embed`）：标识符前缀 = 目标名，只有该目标能看到；
- **工程级**（顶层 `embed`）：供库源使用。

框架自己的嵌入与工程的嵌入**各自生成、按单元分发**——同一个编译单元的命令行上不会
出现两个 `battery/embed.hpp`（那会导致先命中的赢、另一方的资源编译失败）。
跨边界引用对方资源**不受支持**（工程看不到框架的资源，反之亦然）。

## 命令行（工程必须解析）

控制通道是"应用可被智能体驱动"的入口，而端口与控制文件是命令行给的。
框架提供通用解析器，直接用即可（`st init` 的模板已经接好）：

```cpp
#include "st/app/cli.hpp"
st::app::CommonOptions common;
if (auto parsed = st::app::parse_common_options(argc, argv, common); !parsed) { /* 报错 */ }
st::app::Application app("myapp", "0.1.0", common.app);
```

支持 `--headless`、`--backend`、`--width/--height`、`--scale`、`--title`、`--theme`、
`--control-port`、`--control-file`、`--enable-script`、`--frames`、`--ms`。

> 入口推荐用 `ST_MAIN(fn)`（`st/core/entry.hpp`）：Windows 的 `argv` 是 ANSI 编码，
> 中文参数会乱码；该宏在入口处统一转 UTF-8 并设好控制台代码页。

## 构建速度与共享对象缓存

首次构建要编译框架的 ~60 个源文件（含 QuickJS），**每个工程之后**则近乎零成本：

- 缓存位置：`{ST_HOME:-~/.shuangtian}/cache/objects`
- 键 = 编译命令（编译器 + 全部标志/宏/包含目录 + 源路径），**排除工程相关的 `-o`/`-MF`**；
- 框架单元刻意用**框架自己的**标志集编译（不含工程的 `-I`/`-D`，也不吃工程的 PCH），
  这样它的编译命令与引用方无关 → 第二个工程直接命中。
- 想要干净测量：`ST_NO_CACHE=1 st build ...`；关闭缓存另见 `st doctor` 的缓存行。

实测（本仓库规模）：新工程冷构建 ≈ 30 s；**第二个工程 ≈ 4 s**（59/60 个单元命中缓存）。

## 常见问题

| 现象 | 原因与处置 |
|---|---|
| `未找到工程清单` | `project` 指错目录，或还没 `init` |
| `框架路径下没有 st.pkg` | `framework.path` 不是框架根（应是含 `st.pkg` 的目录） |
| 链接缺符号 | 清单里没写 `framework`，或 `sources` 与 target 源重复匹配同一批文件 |
| 交叉编译说"清单未定义工具链" | 框架的 `toolchains` 没被并入 → 确认 `framework.path` 正确（工具链随框架继承） |
| 中文路径打不开 | 用 `st::fs` 的接口（`fs::read_text`/`to_path`），不要 `ifstream(std::string)` |
| 控制通道连不上 | 入口没解析 `--control-port`/`--control-file`（见上文"命令行"） |
