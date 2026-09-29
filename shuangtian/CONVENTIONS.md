# 霜天（Shuangtian）编码约定 — 全现代 C++20，禁用易错特性

本文件是霜天框架的**强制**编码契约。违反即视为缺陷，由三重机制拦截：**① 人工审阅口径（本文档）② 编译器加严警告（`-Werror`）③ 自研 `st lint` 静态扫描**。设计内容见 `DESIGN.md`。

## 0. 一句话

> 只用**现代 C++20 中可静态验证的安全子集**；凡历史上以「易错」著称的语言特性，一律禁用并由工具链强制。

## 1. 语言与工具链

| 项 | 规定 |
|---|---|
| 标准 | `-std=c++20`（GCC 13.3 / Clang 18 实测基线；`<format>`/`<expected>`/concepts/ranges/span 均须可用） |
| 编译器 | `g++` 或 `clang++`（由 `stpm` 直驱，不经 CMake/Make） |
| 第三方依赖 | 框架本体**零第三方依赖**；后续引入的第三方源码一律由 `stpm` 统一管理（见 `DESIGN.md`「包管理」），不得绕过 |
| 系统能力 | 一律**运行时 `dlopen` 可选加载**（X11/Wayland/GL/Vulkan/TLS），缺失即回退或明确报错 |
| 平台分支 | 用 `#if defined(_WIN32)` 等**条件编译指令**（允许），禁止用**函数式宏**做分支 |

## 2. 禁用清单（硬红线）

| # | 禁止 | 必须替代 | 强制手段 |
|---|---|---|---|
| R1 | 裸 `new` / `delete` / `malloc` / `free` / `realloc` | `std::make_unique` / `make_shared` / `std::vector` / RAII 包装类型 | lint + 审阅 |
| R2 | 拥有所有权的裸指针 `T*`（含函数返回、成员、参数所有权） | `std::unique_ptr<T>` / `std::shared_ptr<T>` / `std::span<T>`（非拥有） | lint + 审阅 |
| R3 | C 风格数组 `T a[N]`（数据表除外，见 §3.6） | `std::array<T, N>` / `std::vector<T>` / `std::span<T>` | lint |
| R4 | 裸指针算术 `p + n` / `p[i]` | `std::span::subspan` / `.at()` / 索引受检访问 | 审阅 |
| R5 | C 风格强制转换 `(T)x` | `static_cast` / `const_cast`（禁用于去除 const 写回）/ `std::bit_cast` | `-Wold-style-cast` |
| R6 | `reinterpret_cast` | `std::bit_cast`；系统 API 边界（`void*` 上下文）集中在 `src/*/platform_*.cpp` 单点封装 | lint（白名单：`platform_` 文件） |
| R7 | 函数式宏 `#define F(x)` / 对象式宏做常量 | `constexpr` / `consteval` / `inline constexpr` / `if constexpr` / concepts / 模板 | lint |
| R8 | `goto`、`setjmp`/`longjmp` | 结构化控制流（`for`/`while`/状态机 `switch`） | lint |
| R9 | 未初始化变量、隐式窄化转换 | 统一 `{}` 初始化；显式 `static_cast`；`-Wconversion` | `-Wconversion -Werror` |
| R10 | 异常作控制流（`throw` 自有错误） | `st::Result<T>`（`std::expected`）/ `std::optional` | lint（禁 `throw`，`std::bad_alloc` 由 STL 抛出会终止进程） |
| R11 | 全局可变状态（`static` 非 const 变量、可变单例） | 显式注入 `Context&` 贯穿调用链；常量用 `inline constexpr` | lint（禁止非 const `static` 全局） |
| R12 | `detach` 线程、裸 `std::mutex` 锁/解锁 | `std::jthread` + `std::stop_token`；`std::scoped_lock` / `lock_guard` | lint |
| R13 | 多重继承、虚继承 | 单一继承的最深为「纯抽象接口 + 实现类」；组合取代多继承 | 审阅 |
| R14 | C 风格可变参（`...`）、`printf` 族 | `std::format` / 自研 `st::fmt`（格式化 API） | lint |
| R15 | `union` 与类型双关读 | `std::variant`；位转换用 `std::bit_cast` | lint |
| R16 | `const_cast` 用于写回（破坏 const 正确性） | 重新设计数据流；系统 API 边界单点封装 | 审阅 |
| R17 | 越界/未检查的下标访问（对外 API） | 对外 API 全边界检查；热路径经受控封装 + `assert`/`st::debug_assert` | 审阅 + `--san` |
| R18 | 运算符重载滥用（非数学/容器语义类型） | 仅 `math` 层数值类型与容器语义类型可重载 | 审阅 |
| R19 | 隐式类型转换构造（单参非 `explicit` 构造） | `explicit` 标注 | `-Wexplicit-…` 审阅（lint 检查单参构造） |
| R20 | `std::endl`（无谓 flush）、C 风格 `\n` 拼接大字符串 | `'\n'` + `std::format` | lint |
| R21 | 悬垂引用：`string_view` / `span` 指向临时对象 | 生命周期规则：`string_view` 只能指向具名存储或字符串字面量；`span` 只在同表达式/同函数内使用 | 审阅 + ASan |
| R22 | 无 `virtual` 的基类析构（经基类指针删除） | 多态基类 `virtual ~T() = default;` | `-Wnon-virtual-dtor` |

## 3. 必须使用的现代特性（正向规定）

### 3.1 类型与所有权
- 值语义优先；所有权用 `std::unique_ptr`（唯一）/ `std::shared_ptr`（共享，且只在真正共享处用）。
- 多态接口：`class Backend { virtual ~Backend() = default; ... };` + `std::unique_ptr<Backend>`。
- 非拥有视图：`std::string_view`、`std::span<const T>`。

### 3.2 错误处理
```cpp
template <class T> using Result = std::expected<T, st::Error>;
inline constexpr Result<Unit> ok();          // 无值成功
```
- 禁止抛异常表达业务错误；错误必须经 `Result` 显式向上传播（`std::unexpected` / 自研 `st::Err` 宏化写法在 lint 白名单内）。
- 错误类型：`struct Error { ErrorCode code; std::string message; }`，`ErrorCode` 为 `enum class`。

### 3.3 常量与编译期
- `constexpr` / `consteval` / `constinit` 用于所有可编译期求值的量（颜色、布局常量、设计 token、查表）。
- `static_assert` 校验不变量（如 token 表单调性、结构尺寸）。

### 3.4 泛型
- 模板约束一律用 **concepts**（`template <std::integral T>`），禁止裸 `typename` + SFINAE `enable_if` 做约束。
- 算法优先 `std::ranges`（`std::ranges::sort` / `views::transform`）。

### 3.5 并发
- 线程一律 `std::jthread`（自动 join + `stop_token` 协作取消）。
- 互斥：`std::mutex` + `std::scoped_lock`/`lock_guard`（RAII）；禁手动 `lock()`/`unlock()`。
- 共享数据封装在自带锁的类型里（`class Counter { std::mutex m_; int v_; ... }`），不裸露共享变量。

### 3.6 例外：数据表与 SIMD
- **数据表**（字体表、Unicode 表、图标路径数据、关键字表）允许 `inline constexpr std::array<T, N>`，禁止裸 C 数组，除非是 `std::bit_cast` 目标。
- **SIMD**：仅 `src/raster/simd_*.cpp` 与 `src/core/hash.cpp` 可含 intrinsic 与受控指针运算；文件顶部标注 `// SIMD 边界：本文件是 §2 禁令的受控例外（范围：<说明>）`，且所有访存取 `std::span` + 显式边界。

### 3.7 字符串与格式化
- 一律 `std::string`（拥有）/ `std::string_view`（视图）；禁止 `char*` 做字符串运算（系统 API 边界除外，单点封装）。
- 格式化一律 `std::format`。
- 路径用 `st::Path`（封装 `std::filesystem::path`，隔离平台差异）。

### 3.8 JSON 与外部依赖

**JSON 一律走 `st/ext/json.hpp` 的封装面**，不直接调用 nlohmann 的原生接口：

| 场景 | 允许 | 禁止（会抛异常或语义不保真） |
|---|---|---|
| 解析 | `st::json_parse` / `json_parse_file` | `Json::parse`（抛 parse_error） |
| 写盘 | `st::json_write_file` | `Json::dump` 直接写文件（非法 UTF-8 会抛） |
| 读键 | `json_get_string/i64/double/bool/string_array`、`json_find`、`json_at`、`json_path` | `Json::at()`（L12）、`Json::value()`、const `operator[]`（键缺失是 UB） |
| 查键存在 | `json_find(x,"k") != nullptr`、`x.contains("k")` | —— |
| 构造 | nlohmann 原生（`Json::object()` / `obj["k"] = v` / `push_back`） | —— |

两条容易踩的坑（都有实际事故）：

1. **`at()` 会终止进程**。nlohmann 的 `at()` 在键缺失时抛 `out_of_range`，而本框架**不设通用异常边界**，
   于是它直接冒泡成 `std::terminate`（曾因鼠标事件的修饰键缺席把整个应用干掉）。一律用 `json_at`。
2. **花括号是“造数组”信号**。`Json x{Json::object()}` 会命中 `initializer_list` 构造，
   得到「含一个空对象的数组」而不是对象（曾导致控制通道响应构造失败）。要么用 `=` 拷贝初始化，
   要么用 `Json::object()` 直接初始化。

**第三方源码纪律**（`vendor/`）：

- 只准**原样引入**，不得就地修补——要改就升级版本（否则失去可追溯性）；
- 每个依赖在 `vendor/sources.json` 登记版本/来源/许可/SHA-256/剔除清单，并可用 `sha256sum -c CHECKSUMS.sha256` 校验；
- 第三方翻译单元**不套本工程的告警集（`-w`）、不进 PCH、不做 sanitizer 插桩**——
  我们负责自家代码的质量，不负责上游的；混编不影响 ASan 对我们的检测能力（分配器是全局的）；
- C 源用 `-x c -std=gnu11` 编译（同一个编译器二进制，不引入第二套工具链），标志集与 C++ 分离。

**脚本能力（QuickJS）的姿态**：默认关闭，须显式开启；不提供任何系统访问
（`quickjs-libc.c` 已剔除）；内存/栈/时长/转换深度四重配额在运行时层强制。

## 4. 编译强制集（写进 `st.pkg`）

```text
-std=c++20 -fno-strict-aliasing
-Wall -Wextra -Werror -Wconversion -Wshadow -Wpedantic
-Wold-style-cast -Wnon-virtual-dtor -Wcast-align -Wnull-dereference
-Woverloaded-virtual -Wdouble-promotion -Wimplicit-fallthrough
-Wunused -Wuninitialized -Wsign-conversion -Wformat=2
```
**sanitizer 档**（`st test --san`）：追加 `-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1`。
**优化档例外（已登记）**：`dev`/`release`（优化 + 预编译头）追加 `-Wno-null-dereference`；`san` 档追加
`-Wno-null-dereference` 与 `-Wno-maybe-uninitialized`。原因：GCC 13 在「优化 + 系统头内联 + sanitizer」组合下会对
libstdc++ 内部代码（`vector::insert`、`<regex>` 的 `std::function` 控制块等）产生**误报**，与我们的代码质量无关；
真实缺陷由 `san` 档的 ASan/UBSan **运行期实测**兜住（发现 UB 即视为 bug，见 §7），且 `debug`（-O0）档这些警告仍全开。
每条例外都必须在此登记并写明理由（§9 变更流程）。
**发布档**：`-O2 -DNDEBUG`（禁 `-ffast-math`）。

## 5. 命名与文件

| 项 | 规范 | 例 |
|---|---|---|
| 命名空间 | `st`，分层子命名空间 | `st::json`、`st::raster`、`st::ui` |
| 类型/概念 | `PascalCase` / `PascalCase` | `Canvas`、`Framebuffer`、`FontFace` |
| 函数/变量 | `snake_case` | `fill_path`、`line_height` |
| 常量 | `snake_case`（`inline constexpr`） | `block_gap`、`radius_md` |
| 私有成员 | `name_`（尾部下划线） | `pixels_` |
| 宏 | **不存在**（`#pragma once` + 平台 `#if` 除外） | — |
| 头文件 | `include/st/<层>/<模块>.hpp`，**自包含 + `#pragma once`** | `include/st/raster/canvas.hpp` |
| 实现 | `src/<层>/<模块>.cpp`（与头同名） | `src/raster/canvas.cpp` |
| 测试 | `tests/<层>_<模块>_test.cpp`，入口 `int main()` 用自研 `st::test` | `tests/core_json_test.cpp` |
| 枚举 | `enum class Name`，成员 `PascalCase` 或全小写蛇形（同一枚举内一致） | `enum class BlendMode { SrcOver }` |

## 6. 头文件纪律

1. 每个头文件自包含（单独 include 即可编译），`#pragma once`。
2. 头文件**不引入实现细节**：不 include 系统重量级头（`<windows.h>` 等）——用前向声明 + PIMPL，实现里才 include。
3. include 顺序：本模块头 → 霜天其他头（按层由下到上）→ 标准库（字母序）。
4. 公共 API 必须有 `///` 文档注释（一句话说明 + 前置条件/错误语义）；内部实现注释从简。
5. 所有公共结构体的字段顺序即内存布局约定（跨协议序列化用），变更需同步 `DESIGN.md`。

## 7. 测试纪律

- 每个模块随代码提交单测；**新代码无测试视为未完成**。
- 单测必须可在 `--san` 档（ASan + UBSan）零报错通过；发现 UB 即视为 bug（不是测试问题）。
- 测试不依赖网络/时钟漂移/外部文件（字体测试用系统字体路径探测，缺失则 skip 并打印原因）。
- 测试须可并行：不写共享临时文件（用 `mkdtemp` 等价封装 `st::fs::temp_dir()`）。

## 8. lint 规则（`st lint`，自研实现）

扫描 `src/`、`include/`、`tests/`、`examples/`、`tools/` 的 `.hpp/.cpp`，按行正则匹配，命中即失败（行内 `// lint-allow: <rule-id> 原因` 可豁免，必须写原因）：

| 规则 | 模式 | 豁免 |
|---|---|---|
| L1 | `\bnew\s`、`\bdelete\s`、`\bmalloc\s*\(`、`\bfree\s*\(` | 注释内 |
| L2 | C 风格转换：仅在「行首/空白/`=(,;{!&|?:` 之后」出现 `(类型)` 才算（模板实参与调用实参不误报） | 无 |
| L3 | `#define\s+\w+\s*\(` （函数式宏） | `#define` 在 `#if` 内且非函数式 |
| L4 | `\bgoto\b`、`setjmp`、`longjmp` | 无 |
| L5 | `\bthrow\b` | 测试内的断言宏实现 |
| L6 | `reinterpret_cast` | `platform_*.cpp`、`simd_*.cpp` |
| L7 | `\bprintf\s*\(`、`\bsprintf\s*\(` | 无（CLI/示例改用 `st::print`，见 §9） |
| L8 | 可变全局：**作用域感知**——仅花括号深度 0（命名空间作用域）且形如 `类型 名字 = …/;` 的声明（单行正则无法区分局部变量，故该规则由专用检查实现） | 无 |
| L9 | `\.detach\s*\(`、`\.lock\s*\(`（裸互斥） | 无 |
| L10 | 单参构造缺 `explicit`（提示级，不导致失败） | `// lint-allow: L10 原因` |
| L11 | `\bstd::endl\b` | 无 |
| L12 | `\.at\s*\(`（键缺失即抛异常，而本框架无通用异常边界） | 无（Json 用 `json_at`/`json_find`，容器用 `find`） |

**扫描语义（重要）**：匹配前先做两层净化——① **注释**不参与任何规则；② **字符串字面量内容**不参与任何规则
（关键字表、规则表自身的正则字符串都不是代码，否则 linter 会对着自己的关键词表报几十条"违规"）。
只涉及**代码文本**。

**文件级豁免登记（§9 变更流程要求）**：
| 规则 | 豁免对象 | 理由 |
|---|---|---|
| L6 | `platform_*.cpp`、`simd*.cpp/.hpp` | 系统 API 与 SIMD intrinsics 的位级重解释只能在这里发生（单点封装） |
| L3 | `include/st/test/test.hpp` | 断言宏需要在调用点取得文件/行号与表达式原文，是函数式宏唯一被认可的用途 |
| L10 | 逐行 `// lint-allow: L10 …` | `Result`/`Value` 的隐式值构造是刻意设计（与 `std::expected` 一致） |

> L12 只收 `.at(` 而不收 `.value()`：`value()` 是自有组件的常见 getter 名（`Slider::value()` 等），
> 文本级规则无法区分接收者类型，收进来会天天误报。Json 上的 `.value()` 由 §3.8 的约定与评审把关。

`st lint --explain <rule>` 打印规则详情。lint 亦检查**文件布局**（头/实现同名、目录归属）与**禁用 include**（`<windows.h>`/`<X11/Xlib.h>` 只能出现在 `platform_*`）。

**控制台输出统一走 `st::print`（`st/core/print.hpp`）**：`std::format` 语法且**编译期校验**格式串与实参类型，
从根本上消掉 printf 的格式串缺陷（L7 因此可以全局禁止而不留后门）。

## 9. 变更流程

1. 任何新增的「例外」（如新的 SIMD 重路径）必须先在本文档登记。
2. 新增公共 API → 同步 `DESIGN.md`。
3. 改动协议/清单格式/设计 token → 先改文档再改代码，并升版本号。
