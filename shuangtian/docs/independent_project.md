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

## 元素 id：动态数据请给 `key`

界面元素的 id 是**外部引用它的唯一凭据**（协议 `get/set/invoke`、选择器 `#id`、脚本）。
自动 id 默认是路径式的（`tasks/ListItem[2]`）——索引会随插入/删除整体位移，于是"刷新后
原来看中的那一项变成别的数据"。动态数据请用 `set_key`（或容器的 key 同步接口）标出业务身份：

```cpp
std::vector<st::ui::List::Entry> rows;
for (const auto& task : tasks) {
  st::ui::List::Entry row;
  row.key = task.id;          // 业务身份（不是显示文案）
  row.label = task.title;
  rows.push_back(std::move(row));
}
list->sync_items(rows);        // 同 key 的项沿用同一元素与同一 id；选中态也跟着 key 走
```

- id 变成 `tasks/ListItem@<key>`，与位置无关；文案改了 id 不变。
- **不要**用 `clear_items()` + 逐个 `add_item()` 做刷新：索引推倒重来、选中态丢失。
- `key` 里可以有空格/点等业务字符，框架会转义成选择器安全形式（`task 42.v2` → `task-42-v2`）。
- 同一父节点下 `key` 不要重复（重复会告警，因为 id 会撞车）。

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
| 链接缺 `sqlite3_column_table_name` / `sqlite3_column_origin_name` | 框架的 C 标志（`-include st_sqlite3_config.h`）没走到框架单元——这是 2026-10-05 修掉的缺陷，**升级框架后自动消失**；若在两版本之间碰到，报因就在 `make_framework_flags` 的 `framework_c_flags` 形参（详见下文“只在这条路径上暴露的缺陷”）。 |
| 应用启动即退、等不到控制通道就绪 | 开了应用自己的 argv 解析而**漏了共享 CLI**（`st::app::parse_common_options`）；或共享 CLI 不认驱动方传的某个参数（如 `--shots`）——错误在应用日志里是一句“未知参数”。 |
| 交叉编译说"清单未定义工具链" | 框架的 `toolchains` 没被并入 → 确认 `framework.path` 正确（工具链随框架继承） |
| 中文路径打不开 | 用 `st::fs` 的接口（`fs::read_text`/`to_path`），不要 `ifstream(std::string)` |
| 控制通道连不上 | 入口没解析 `--control-port`/`--control-file`（见上文"命令行"） |

## 只在这条路径上暴露的缺陷（为什么框架单测全绿也拦不住）

“引用 framework 的独立工程”与“框架自己构建”**不是同一条路径**，有三处差异会让缺陷
只在前者显形。三处都已在 2026-10-05 修复，写在这里是因为**它们为什么当时抓不到**比缺陷本身更值得记住：

1. **框架的 C 标志被调用方工程的顶掉**：`make_framework_flags` 曾用调用方的
   `manifest.c_flags`（独立工程一般是**空**的）而非框架的。于是框架单元里唯一的 C 源
   （`sqlite3.c`）丢了 `-include st_sqlite3_config.h` ⇒ `SQLITE_ENABLE_COLUMN_METADATA`
   未定义 ⇒ `sqlite3_column_table_name` **链接期** undefined reference。
   **为什么框架自己构建不报**：那时 `manifest` 就是框架清单，两者恰好相等——自建一百次都不复现。
2. **共享 CLI 不认 `--shots`**：驱动方启动应用的固定契约含它，落进 `else` 分支 ⇒
   `Invalid` ⇒ 模板 `main` 打印错误并 return 1 ⇒ “应用起不来”。
3. **框架源缺头**（`<cstring>` / `<format>`）：在新编译器/新 SDK 上才报，
   而框架自己构建时碰巧有间接包含。

**防复发机制（已建）**：`tests/pkg_integration_test.cpp` —— 真建一个引用 framework 的
最小工程、真构建、真跑起来（判据是**产物能不能跑**，不是“函数返回了 ok”：
缺陷 1 在编译期零症状，任何不走到“链接 + 执行”的检查都会漏掉它）。
由 `st test` 随全套单测一起跑；`ST_INTEGRATION_BUILD=0` 可关（关掉时明确跳过，不假绿）。
