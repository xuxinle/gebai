# 霜天（Shuangtian）编码约定 — 全现代 C++20，禁用易错特性

本文件是霜天框架的**强制**编码契约。违反即视为缺陷，由三重机制拦截：**① 人工审阅口径（本文档）② 编译器加严警告（`-Werror`）③ 自研 `st lint` 静态扫描**。设计内容见 `DESIGN.md`。

## 0. 一句话

> 只用**现代 C++20 中可静态验证的安全子集**；凡历史上以「易错」著称的语言特性，一律禁用并由工具链强制。

## 1. 语言与工具链

| 项 | 规定 |
|---|---|
| 标准 | `-std=c++20`（**GCC 13.3 / 16.2** 与 **Clang 23** 实测基线；`<format>`/`<expected>`/concepts/ranges/span 均须可用） |
| 编译器 | 由 `stpm` 直驱（不经 CMake/Make）：**g++ 优先，clang++ 次之**——两者是**同一条口径**（GCC 风格标志 + `-MMD` 依赖 + `-l` 链接 + libstdc++ 运行库），所以构建系统只有一套命令，不按编译器族分叉。Windows 自举用 `bootstrap.ps1`（g++ 优先，MSVC 回退；对应 `bootstrap.sh`） |
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

**第三方源码纪律**（`third_party/`）：

- 只准**原样引入**，不得就地修补——要改就升级版本（否则失去可追溯性）；
- 每个依赖在 `third_party/SOURCES.md` 登记版本/来源/许可/SHA-256/剔除清单，并可用 `sha256sum -c CHECKSUMS.sha256` 校验；
- 第三方翻译单元**不套本工程的告警集（`-w`）、不进 PCH、不做 sanitizer 插桩**——
  我们负责自家代码的质量，不负责上游的；混编不影响 ASan 对我们的检测能力（分配器是全局的）；
- C 源用 `-x c -std=gnu11` 编译（同一个编译器二进制，不引入第二套工具链），标志集与 C++ 分离；
- **上游大块头（SQLite）的编译期配置写在我们自己的头里**（`third_party/sqlite/st_sqlite3_config.h`），
  清单的 `c_flags` 以 `-include` 前置它——上游文件保持一字未改，
  「我们动了上游什么」因此是一份可读的 diff（见 `docs/sqlite_integration.md`）。

**脚本能力（QuickJS）的姿态**：默认关闭，须显式开启；不提供任何系统访问
（`quickjs-libc.c` 已剔除）；内存/栈/时长/转换深度四重配额在运行时层强制。

**数据库（SQLite）的姿态**：源码内置（amalgamation）；**公共头不外泄任何 `sqlite3_*` 符号**
（`include/st/ext/database.hpp` 只前置声明，实现文件是唯一包含 `sqlite3.h` 的翻译单元）；
一切失败经 `Result` 返回；错误码 → 框架错误分类的映射只有一处（`map_sqlite_code`）并由测试逐条断言；
**编译期开关集中在 `third_party/sqlite/st_sqlite3_config.h`**（经 `st.pkg` 的 `c_flags` 以 `-include` 前置）——
禁止就地修改上游三件套；**不提供运行时加载原生扩展**（`SQLITE_OMIT_LOAD_EXTENSION`），
与上述脚本层姿态一致：可执行代码入口必须显式设计。见 `docs/sqlite_integration.md`。

**脚本层（`ui::ScriptHost`）的编码约定**：

1. **不得逐属性跨语言边界**。JS 侧读写走快照 + 变更集（`$('#x').set()` 只入队，阶段末一次提交）；
   新增宿主桥函数时先问"这是一次记账还是 N 次往返"。
2. **组件操作必须走 `st/ui/actions.hpp`**（`element_snapshot`/`apply_properties`/`invoke_element`）——
   禁止在脚本层或协议层另写一份。三条路径语义必须一致（已因"三份实现"踩过两次）。
3. **新增组件必须实现属性面**（`get_property`/`set_property`/`property_names`）：
   `apply_properties` 声称支持的名字（`text`/`value`/`checked`/…）若组件没实现，表现是**静默无效**。
   这一条有实际教训：`Input` 长期缺属性面，协议 `set {value}` 一直无效。
4. **选择器未命中不抛异常，但必须可观测**（记日志）；
   否则拼错选择器（最常见的脚本错误）会表现为"改了没反应"。
5. 事件观察者通知一律**在元素自身处理之后**（顺序错了会让脚本写入被 C++ 处理器覆盖）。

**编译期资源嵌入（battery::embed）**：资源用真实文件编写（`assets/`），清单声明 `embed`；
不要为了"省事"把大段文本塞成 C++ 原始字符串——那正是 embed 要解决的问题。

### 3.9 交互元素：动作一律放在 `activate()`

**可激活元素（按钮、列表项、开关项……）的"动作"必须实现在 `virtual void activate()` 里**，
`on_event` 收到 `Click` 时调用 `activate()`，不要在 `on_event` 里直接写动作逻辑。

理由：`activate()` 是**所有激活路径的唯一汇聚点**——鼠标点击（`on_event`）、协议
`invoke(click)`（`Element::invoke_action`）、脚本 `ui_invoke` 全都走它。把逻辑写在 `on_event` 里
会导致"真实点击有效、`invoke(click)` 静默无效"（实测踩到：列表项点不动，而界面看起来一切正常）。
对"应用可被智能体驱动"来说这是致命的——自动化流程恰恰只能走 `invoke`。

配套约定：
- 元素若能被点击激活，就**必须**覆盖 `activate()`（`ListItem` 由此从"只在 on_event 里处理"改过来）。
- 容器类元素（如 `List`）若要"按新数据重建子项"，提供 `clear_*` + `add_*` 这类成对的接口；
  只能追加不能清空会让过滤/刷新无法实现（`List::clear_items` 由此而来）。
- 元素的自动生成 id 必须**稳定、唯一、无控制字符**：选择器、协议消费方、脚本层都依赖它
  （动态数据项的具体做法见 §3.10）。

### 3.10 元素身份：`id` 给名字，`key` 给身份

ID 是**外部引用元素的唯一凭据**（协议 `get`/`set`/`invoke`、选择器 `#id`、脚本 `$('#id')`）。
因此不准把"位置"当身份：

- 动态数据（列表、表格、标签页、树）里的一项，**必须**用 `set_key(业务身份)` 标出它是谁
  （任务 id、行主键、配置项名）；文案变了、位置变了，id 不变。
  自动 id 形如 `tasks/ListItem@task-42`；无 key 时才回退到 `Type[index]`。
- 刷新数据用容器的**按 key 同步**接口（如 `List::sync_items`），**不要**用 `clear_*` + 逐个 `add_*`
  重建：后者索引推倒重来，外部按 id 引用会错位、选中态静默丢失。
- 同一父节点下 `key` 不得重复（会告警）：撞车即自动 id 撞车，按 id 查找可能命中另一个元素。
- id 会进入选择器，而选择器在 `#`/`.`/`:`/`[`/空白 处切词；key 由框架转义成安全字符，
  但**不要**把 id 拼成依赖这些字符的样子。

### 3.11 自绘组件契约（坐标系、状态成员、焦点）

**坐标系**：`paint_content(context, canvas)` 拿到的是**视口绝对坐标系**的画布——
自绘必须从 `bounds_.x/y` 起算（`canvas.fill_rect({bounds_.x + …})`），不得按局部坐标画。
按局部坐标画会整块位移（实测：自绘树按局部坐标画 → 上移 70px 压住标题、标签画进顶栏）。
基线接口（`paint_box`/`paint_text`/子节点绘制）已按 `bounds_` 落位，只有自己的几何需手动偏移。

**状态成员**：组件**不得**声明与 `Element` 保护成员同名的成员（`style_` `bounds_` `id_` `key_`
`visible_` `enabled_` `focusable_` `hovered_` `pressed_` `focused_` `dirty_` …）——那是
**遮蔽**（shadowing），而编译器**不给任何警告**：焦点写基类、读遮蔽副本时，症状是
「功能静默失效但测试全绿」（`CodeEditor` 自带 `bool focused_{false}` → 光标永不绘制、
括号高亮失效；而直接调 `set_focused` 的组件级单测读写落在同一侧，全部通过）。
确有独立语义时**改名**并显式桥接（如行组件的显示标签叫 `label_`：基类 `key_` 是**稳定
逻辑身份**、参与自动 id，不是显示文案）。守规则：lint `L13`。

**语义标志**：`semantics_flags()` 覆写必须以 `SemanticsFlags flags = Element::semantics_flags();`
起手再覆写特有字段。以 `SemanticsFlags flags{}` 重建会丢掉基类的
visible/enabled/focused/hovered/pressed —— 焦点经 `UiRoot::set_focus` 设置时，
控制通道语义树与 `:focused` 选择器恒报 false。守规则：`L13`。

**焦点**：
- 文本编辑类（`Input`/`TextArea`/`CodeEditor`）构造函数就 `set_focusable(true)`：
  点击聚焦路径是 `hit_test → focusable() && set_focus`，而 `activate()`（Enter/Space）
  又要求先有焦点——默认 `false` 是个死循环：**点击永远聚焦不了编辑器**。
- 焦点一律经 `UiRoot::set_focus` 设置（只改元素自己的 `focused_` 标志会让后续
  `input.text`/`input.key` 无处可送）；它按 `focusable()` **严格裁决**：不可聚焦元素
  拒绝接受焦点并返回 `false`，调用方（控制通道/脚本）必须如实上报。
  `focusable()` 是「能否持有焦点」的契约，Tab 焦点环按它筛选；无条件赋值会产生
  「root 焦点指向它、键盘派发给它、Tab 环跳过它」的状态分裂。
  （例外：浮层不靠焦点拿键盘——`UiRoot` 把 KeyDown 先派给可见浮层，因此
  `SelectPanel`/`MenuPanel` 即使不可聚焦也能收键盘。）

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
| 测试 | `tests/<层>_<模块>_test.cpp`，入口 `int main()` 用自研 `st::test` | `tests/ext_json_test.cpp` |
| 枚举 | `enum class Name`，成员 `PascalCase` 或全小写蛇形（同一枚举内一致） | `enum class BlendMode { SrcOver }` |

## 6. 头文件纪律

1. 每个头文件自包含（单独 include 即可编译），`#pragma once`。
2. 头文件**不引入实现细节**：不 include 系统重量级头（`<windows.h>` 等）——用前向声明 + PIMPL，实现里才 include。
3. include 顺序：本模块头 → 霜天其他头（按层由下到上）→ 标准库（字母序）。
4. 公共 API 必须有 `///` 文档注释（一句话说明 + 前置条件/错误语义）；内部实现注释从简。
5. 所有公共结构体的字段顺序即内存布局约定（跨协议序列化用），变更需同步 `DESIGN.md`。
6. 每个 `.cpp`（翻译单元）**自包含**：用到哪个标准库头就 `#include` 哪个，**不得依赖** `st/pch.hpp`
   或其他头的传递包含。这不是洁癖——PCH 会把缺失暴露延迟到「首次自举」那一刻，理由与实测见 §10.3 第 3 条。

## 7. 测试纪律

- 每个模块随代码提交单测；**新代码无测试视为未完成**。
- **用例的耗时/成败若取决于环境而非代码，必须用 `ST_TEST_SLOW` 标出来**（默认不跑）：
  量帧耗时/吞吐阈值的性能门禁、需真编译真跑进程的集成用例。它们占测试壁钟近三分之一，
  且在共享机器上**两个方向都会失败**（机器慢→超阈值，机器快→余量超界）——
  实测同一个二进制相邻两轮分别是 `frame_cost_*` 与 `bench_*` 变红。
  把它们留在默认路径上，就是给每次迭代随机撒红灯（而结论几乎总是“环境”）。
  **组件功能与不变量用例不属于这里**（它们快且确定，必须留在默认路径）。
- 单测必须可在 `--san` 档（ASan + UBSan）零报错通过；发现 UB 即视为 bug（不是测试问题）。
- 测试不依赖网络/时钟漂移/外部文件（字体测试用系统字体路径探测，缺失则 skip 并打印原因）。
- 测试须可并行：不写共享临时文件（用 `mkdtemp` 等价封装 `st::fs::temp_dir()`）。

### 7.1 修缺陷时新写的回归测试：必须验证它真能抓住那个缺陷

**做法：把修复回退、看它变红，再恢复修复、看它变绿。**

不做这一步就会交付一个“恒绿的测试”——它看起像回归护栏，实际什么也拦不住。
这不是假设：修 `SplitView` 分隔线偏移时，第一版回归测试在**回退修复后依然是绿的**。

两个具体的假绿成因（已经踩过，写测试时先自查）：

- **夹具落在“退化点”上**：被测对象在原点/默认值/空集合时，差异与无差异等价
  （`bounds_.x == 0` 时“坐标多加了一次”与“没加”完全等价）。
  测几何/坐标类问题，夹具必须把对象推到**非退化**位置。
- **观测粒度粗于被测特征**：扫一条 1px 发丝线却用 2.0 的步长，会整条跳过，
  扫出空集反而“通过”（前置断言成了帮凶）。扫描步长必须**细于特征尺寸**。

另四条相邻经验：

- **判据别用“精确等于某个常量色”**：绘制往往在底色上做过 alpha 混合，
  落盘像素不等于主题色（实测 `theme.border = D3DCE9` 而实际像素是 `D6DFEB`）。
  用“与背景不同”或“落在预期区间”更稳。
- **像素级断言先聚簇再取值**：画面里可能还有别的非背景像素，
  直接拿“首个/末个命中”的均值当特征位置会被完全带偏（实测差了 278.9px）。
- **判据要选“与被测机制同构”的那个量**：判例——验证“按墨迹居中”时，
  用“墨迹中心 vs 盒中心”：它直接对应被测的那件事。
  而“字在不在盒里”这类弱判据，把居中改成行盒居中也是绿的。
- **断言不能先读可能不存在的字段**：nlohmann 的 `operator[] const` 缺键是 **UB
  （断言崩溃）**。曾经写“先读 `reply["error"]` 再查 `ok`”，于是回退实现后
  响应里没 `error` 字段，测试**直接崩**而不是优雅变红——逆向验证脚本判它失败，
  而真正的问题是断言写法不稳。顺序永远是：先 `ST_CHECK(!ok)`，再 `if (contains(...))`。

### 7.2 涉及桩/替身的契约，桩必须复现被测的那一段行为

**被测的如果是“接线”（谁把值传给谁），而桩把两者的差异抹平了，那条测试就是死的。**

实测（2026-10-06，`capture` 的 `pixel_size`）：契约是“响应的尺寸必须来自**导出结果**，
不能拿请求参数算”。测试桩的 `capture_*` 直接把 `region` 的宽高回回去，
于是**请求值恒等于实际值**——把一个“从 `region` 自己算尺寸”的错误实现（就是原缺陷）
也会判绿。回退验证当场打脸：三条断言全过。

修法是让桩**像真实实现一样夹取**（帧 64×48，与帧缓冲求交），并补上关键用例：
“请求 30×30 落在右下角 ⇒ 实际只能拿到 14×8”。
于是“算的”与“拿的”在这个夹具下**必然不等**，契约才真的被钉住。

自查口诀：**回退修复后测试变红吗？** 不变红就说明夹具把差异抹平了，
而不是“这个缺陷不值得测”。

### 7.3 逆向验证：把“回退必须变红”脚本化

`tools/theme_reverse_verify.py`：逐条把关键改动**临时回退** → 跑对应用例 →
断言它变红 → 恢复。覆盖对象：主题令牌、主题文件（`--theme-file`/`ST_THEME_FILE`）、
悬浮态、文字垂直居中、卡片边框像素对齐、`capture` 区域与尺寸。
**条目数不写死在文档里**（会随改动漂移）——脚本每轮自报「N 条全部通过」。

**改这脚本时要注意两个自陷阱**（都踩过）：

- **回退文本必须能编译过**。曾经把回退写成“注释掉赋值”，
  于是变量变成 set-but-unused，`-Werror` 把编译打断，测试跑的是**上一份二进制**
  → 得到一条假绿。回退时保留引用（`(void)var;`）或改写成等价可编译形式。
- **恢复文件不能只 `copy2`/`move`**。本仓库的增量构建按 **mtime** 判定是否重编，
  而 `copy2` 会把备份时**原有的旧 mtime 一起搬回来**——比刚才编译回退版生成的
  `.o` 还旧，下一轮构建就认为源码未变、**继续用回退版的二进制**。
  现象很隐蔽：单跑绿、全量跑红（全量编到了回退码，而单跑因缓存命中骗过）。
  正确做法：**删旧文件再写新内容**（拿到当前时间），见 §7.4。

### 7.4 改完源码后拿到的测试结论不可信时，先怀疑增量构建

本仓库的增量构建按 **mtime** 判定是否重编（`source_time > object_time`）。
于是三类操作会制造“源码已是修复版、二进制还是回退版”的假象：

| 操作 | 为何踩坑 | 正确做法 |
|---|---|---|
| 逆向验证脚本 `copy2` 恢复 | 把旧 mtime 搬回来，比 `.o` 还旧 | 删旧文件再写（`os.remove` + `open(...,'w')`） |
| `git checkout`/`git stash pop` 恢复 | 同上：git 写文件时用的是索引里的 mtime/或瞬间写入，可能不新于 `.o` | 同上；或改完 `touch` 一下 |
| 手改后立即跑 `st test` | 判定本身没错，但**过滤跑单个用例**会因缓存命中而看着“没重编” | 看构建日志的“重编 N / 命中缓存 M” |

**`st test --force` 只强制“跑测试”，不触发重编**——想强制重编要用
**`st build <target> --profile <档> --force`**。

判据：拿到可疑结论时，先看那一轮构建日志的**重编数**；
与之无关的改动也能看出异常（改 1 个文件却“重编 3”说明有其他源在变）。
实在存疑就 `st build --force` 全量重编一次拿基准。

> 这一条曾经让本会话连续误判三轮（包括一次把回退值当成修复值去判“修复无效”），
> 代价远高于多跑一次全量构建。

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
| L8 | 可变全局：**作用域感知**——① 全局作用域（花括号标签栈只由 namespace 组成，含匿名/嵌套）里的可变声明；② 非 const 的函数内局部 `static`（同为进程生存期共享状态）。类成员不判；含 `const`/`constexpr`/`consteval` 不判（单行正则无法区分局部变量，故该规则由专用检查实现） | ① 行内 `// lint-allow: L8 原因`；② **前置作用域豁免**：`// lint-allow: L8` 写在命名空间开括号前，往后 50 行内生效（整块登记一次，`log.cpp`/`test_runner.cpp` 的既有惯例） |
| L9 | `\.detach\s*\(`、`\.lock\s*\(`（裸互斥） | 无 |
| L10 | 单参构造缺 `explicit`（提示级，不导致失败） | `// lint-allow: L10 原因` |
| L11 | `\bstd::endl\b` | 无 |
| L12 | `\.at\s*\(`（键缺失即抛异常，而本框架无通用异常边界） | 无（Json 用 `json_at`/`json_find`，容器用 `find`） |
| L13 | 组件**遮蔽** `Element` 保护成员（`focused_`/`key_`…）；`semantics_flags` 覆写以 `SemanticsFlags flags{}` 起手 | 无（专用检查：需判断「当前类是否继承 `Element`」与「是否在覆写体内」，单行正则表达不了；同类先例 L8） |
| L14 | `#include <windows.h>`/`<unistd.h>`/`<dlfcn.h>`/`<shellapi.h>`/`<winsock2.h>`/`<arpa/inet.h>`/`<netinet/in.h>`/`<sys/socket.h>`/`<poll.h>`/`<fcntl.h>`，以及直接用 `dlopen`/`dlsym`（§10 第 1 条：平台差异只能出现在 `platform_*`） | `platform_*.cpp`（单点封装）；`shell/backend.cpp`（需 `dlopen` 做**运行时后端探测**本身）；清单 `lint.exempt.L14` 登记的 `core/entry.cpp`（见下表） |

**扫描语义（重要）**：匹配前先做两层净化——① **注释**不参与任何规则；② **字符串字面量内容**不参与任何规则
（关键字表、规则表自身的正则字符串都不是代码，否则 linter 会对着自己的关键词表报几十条"违规"）。
只涉及**代码文本**。

**文件级豁免登记（§9 变更流程要求）**：
| 规则 | 豁免对象 | 理由 | 通道 |
|---|---|---|---|
| L6 | `platform_*.cpp`、`simd*.cpp/.hpp` | 系统 API 与 SIMD intrinsics 的位级重解释只能在这里发生（单点封装） | 内置 |
| L3 | `include/st/test/test.hpp` | 断言宏需要在调用点取得文件/行号与表达式原文，是函数式宏唯一被认可的用途 | 内置 |
| L3 | `include/st/core/entry.hpp`（`ST_MAIN`）| 需在调用点生成 `main` 并正规化 `argv` 编码（Windows 的 `argv` 是 ANSI）。宏而非函数是语言限制：`main` 的签名与返回语义只能在调用点展开 | 内置 |
| L10 | 逐行 `// lint-allow: L10 …` | `Result`/`Value` 的隐式值构造是刻意设计（与 `std::expected` 一致） | 行内 |
| L8 | `src/core/log.cpp`（日志级别/sink/listener）、`src/test/test_runner.cpp`（测试注册表） | 二者是**进程级基础设施**：日志 sink 与测试注册表按设计全局唯一，注入 Context 无处可注（调用方是整个进程）。已在命名空间开括号前用前置作用域豁免整块登记 | 行内（前置） |
| L8 | `src/raster/platform_d3d11.cpp` 的 `g_live_canvases` | 进程级诊断计数器（跨设备/跨线程的 GPU 画布存活数），已用 `atomic` | 行内 |
| L8 | `src/ui/dsl.cpp` 的 `active_composers` | State 写在重组之外时靠它定位订阅者，**必须跨 Composer 实例可见**；已加锁并快照遍历（见 A2 修复） | 行内 |
| L8 | `src/ui/components/code_editor.cpp` 的 `editor_clipboard` | 进程级剪贴板是**刻意的跨实例共享语义**（同一应用内复制到另一处粘贴）；访问点已收敛到单一函数 | 行内 |
| L8 | `src/ui/icon.cpp` 的 `svg_registry` | 图标 sprite 表按设计全局唯一（启动时装载一次，所有 `IconView` 共享），只持有不可变源文本与位图缓存 | 行内 |
| L8 | `tests/ui_dsl_test.cpp` 的用例级计数器 | 需被 lambda 捕获并跨重组共享；作用域限于单个用例（不跨用例泄漏） | 行内（前置） |
| L14 | `src/core/entry.cpp` | `ST_MAIN` 需在调用点正规化 Windows 的 ANSI `argv`（`<windows.h>`/`<shellapi.h>`）+ POSIX 侧 `<unistd.h>`。这是**进程入口**的职责，不属于任何 `platform_*` 横切层 | **清单**（`st.pkg` 的 `lint.exempt.L14`） |
| L14 | `src/shell/backend.cpp` | 需 `dlopen` 做**运行时后端探测**——而这正是 §10 第 1 条「平台后端一律运行时探测（不产生链接期依赖）」的实现处：探测本身就是平台差异的唯一判定点 | 内置 |

> 两条豁免通道都**各自计数**（`st lint` 汇总行报出「行内 N / 清单 M」），不允许静默。
>
> 附带一条：`thread_local` **不在** L8 判据内——它是“每线程一份”，正是**避免**共享可变状态的手段，
> 不应被惩罚（`dsl.cpp` 的 `tls_composer` 即正确用例）。

> L12 只收 `.at(` 而不收 `.value()`：`value()` 是自有组件的常见 getter 名（`Slider::value()` 等），
> 文本级规则无法区分接收者类型，收进来会天天误报。Json 上的 `.value()` 由 §3.8 的约定与评审把关。

`st lint --explain <rule>` 打印规则详情。lint **确实**检查**禁用 include**（L14：系统头只能出现在 `platform_*` 与已登记豁免里）——
这条自 2026-10 起由规则实现（此前仅是本节的纸面断言，见 `docs/BACKLOG.md`）。

> L8 的判据在 2026-10 收紧过一次（旧实现只判花括号深度 0，而本工程几乎每个文件的辅助全局都在
> **匿名命名空间**里，于是全部漏网——包括 `src/ui/dsl.cpp` 那个无锁遍历的 `active_composers`）。
> 收紧后同一次扫描从“0 违反”变成 6 条真问题，均已登记豁免或修复；这印证了“写着已检查、实际没检查”
> 比“不检查”更危险。

**控制台输出统一走 `st::print`（`st/core/print.hpp`）**：`std::format` 语法且**编译期校验**格式串与实参类型，
从根本上消掉 printf 的格式串缺陷（L7 因此可以全局禁止而不留后门）。

## 9. 变更流程

1. 任何新增的「例外」（如新的 SIMD 重路径）必须先在本文档登记。
2. 新增公共 API → 同步 `DESIGN.md`。
3. 改动协议/清单格式/设计 token → 先改文档再改代码，并升版本号。

### 9.1 视觉改动的纪律（颜色 / 阴影 / 尺度）

颜色是唯一无法靠代码审查发现问题的领域：`#94A3B8` 与 `#64748B` 在 diff 里看不出差别，
而前者在白底上只有 2.6:1（11px 小字直接看不清）。因此：

| 项 | 规定 |
|---|---|
| 颜色字面量 | 设计令牌用 `Color::from_rgba_hex`（显式 8 位 `0xRRGGBBAA`），**不用 `from_hex`**：后者靠“数值 > 0xFFFFFF”猜格式，带前导零的 RGBA 会被静默误读（`0x0000008C` → 纯蓝不透明）。回归：`tests/math_color_test.cpp` |
| 对比度 | 文字/背景、语义色/卡片面、`on_primary`/`primary`、焦点环/各底色都必须过 `tests/ui_theme_test.cpp` 的下限（正文 4.5:1、辅助小字 4.0:1）；运行期需要兜底时用 `math::ensure_contrast` |
| 改调色板/尺度 | 先跑 `st test theme_ color_`——把“好不好看”变成可回归的断言，而不是靠评审口味 |
| 新增自绘图形 | 必须有**像素级测试**（如输入框图标槽位、焦点环、描边完整性）：只要能画出来就能数像素 |
| 阴影 | 用 `shadow_sm/md/lg` 三档（两层：关键 + 环境），不自行拼单层阴影；`Element::paint_box` 先环境后关键 |
| 只改“看着怎样”的改动 | **都要先量再改**：用户报的现象（“边缘不平滑”“文字没居中”）是现象不是诊断，先把它翻成一个可量的量（峰值覆盖率 / 墨迹中心偏移），再动手。实测三轮（悬浮态、文字居中、capture 尺寸）**每一轮都是量了才找到根因** |

#### 9.1.1 从像素里量几何：三条判据口决

回看本会话三轮里写坏又改对的量法（都是先量错、才发现量法本身有 bug）：

- **判据要自适应底色，不要写死阈值**。曾用“像素暗于 70 = 文字墨迹”找文字位置，
  而次要按钮的底色本身就是 `#15151a`（全暗）→ 整个按钮区被当成墨迹，
  量出的偏差 +6px 全是假的。改为“与该处底色差异 > 90”后才对。
  另：底色要**就地采样**（如取该行左端的填充色），不能从主题变量里推——
  组件会把主题色再混合一遍。
- **取样区必须避开自己制造的边界**。加了 6px 边距再在边距里取样 → 取到的就是底色，
  我曾据此误判“`capture` 的 region 坐标偏了”。取几何特征时先把内缩开，
  并确保取样区**全部落在目标内部**。
- **同一现象的多个实例差异应当一致**。修文字居中后，5 个按钮的改善量**都是 −2.00px**——
  这说明修的是同一个系统性偏差。若四处改善量各异，那多半是量法在不同底色/尺寸上
  失效了，而不是真有四个 bug。这是很廉价的自检。


### 9.2 观感类改动的纪律（字体渲染 / 配色 / 动画 / 手感）

**验收标准长在用户身上**，而不是测试红绿——这类改动与普通缺陷修复**不是同一种工作**。
完整方法（含四次返工换来的条目、量尺三坑、可复用循环）见
**`docs/perceptual_changes.md`**。写这类改动前必须遵守以下硬约束：

| 约束 | 理由（都是实测代价） |
|---|---|
| **至少两个正交量同向**才可宣布"更好" | 一个指标上涨时，另一个正交量可能在恶化（"更锐"与"更均匀"是两个问题，曾连做三项改进、指标全涨而用户每次都说更差） |
| **报告数字必须注明统计量**（主口径：总墨量之比；中位数作离散度并列） | 同一批数据，中位数 0.92 与总量比 0.96 会给出**相反方向**的结论 |
| **参照取用户实际那条链路** | 无头浏览器不带 GPU 合成，据此得出的方向恰好相反，被用户当场驳回 |
| **参照与待测必须同模式** | 灰度 vs 亚像素差 5%，那是模式差不是质量差 |
| **改默认值 = 产品决策，先给候选+数据由用户拍** | 默认值是所有应用的观感；拍完把依据数字写进注释 |
| **应用默认档抽成可被测试引用的常量，实现与测试走同一入口** | 曾出现"档位只接进渲染器与探针、应用没接"，而测试全绿（`st::app::ClassGammas` + `apply_class_gammas()` 即为此设） |
| **量尺先自检**：能分辨已知差异、窗口只含本行且不小于本行墨迹、各档产物文件名可区分 | 实测：紧凑行框裁掉最宽笔画（0.929 vs 真实 0.992）；大窗口吃邻行墨（整档不可用）；探针固定文件名导致后跑覆盖前跑 |
| **结论与观感冲突时先怀疑尺子** | 本仓库每一次"用户说更差"都对应一次口径缺陷，而不是用户看错 |
| **判据必须带外部参照**（"更锐/更暗/更快了"都不算结论） | **代价最大的一条**：字形网格拟合在**单指标、无参照**下验收（"边缘落整数网格比例 90.9%→21.8%，比之前锐"），据此成了默认档跑了数月。补上真窗口浏览器参照后才发现——**浏览器本身就在"不拟合"那一档**，拟合开其实**过锐**（边缘柔度 2.81 vs 参照 3.22）。"比之前好"与"达到参照"是两件事，前者不需要参照就能量，后者必须 |

## 10. 跨平台强制约束（写代码时逐条自查）

目标平台 **Linux / Windows / macOS**，并支持**交叉编译**。跨平台不是收尾工作：
**一处平台假设会让整个目标平台编不过或运行期出错，而在本机（Linux）完全看不出来。**
以下与 §2 的禁令同等地位（**详细版、反例与提交前自查清单：`docs/cross_platform.md`**——那份用独立编号，不与本节的 10.x 一一对应，见其开头说明）。

1. **平台差异只能出现在 `platform_*` 里**：系统头（`<windows.h>`/`<unistd.h>`/`<dlfcn.h>`…）、
   平台宏、平台 `char*` API 边界。其余代码**不得**直接 `#include <unistd.h>`、
   调 `getpid()/fork()/dlopen()`。需要这类能力时先在后端加**平台无关封装**（如
   `process::current_id()`），再在平台文件里实现两侧。
2. **路径一律 UTF-8，进出都经 `st::fs`**：不手写 `base + "/" + leaf`（Windows 用反斜杠、有盘符/UNC）；
不用 `std::ifstream(std::string)` 直接构造（Windows 上按 **ANSI** 解释，中文路径必坏）——
走 `fs::read_text/read_bytes/write_text` 或 `fs::to_path()`/`fs::to_utf8()`。
**`st::fs` 的转换函数一律不得抛异常**：路径字符串的来源极杂（PATH/环境变量/外部清单/文件名），
Windows 上 `std::filesystem::path` 的两个方向都会抛（UTF-8→宽在字节非法时、宽→窄在代码页
表达不了时），而调用点全是“查文件存不存在”这类纯查询——一抛就是 `terminate`，
表现为“工具无声崩掉、连错误信息都没有”。已在两侧加了不抛的回退，并有回归测试
（`tests/core_fs_test.cpp`）。同理：**列目录/遍历一律用 `generic_u8string()`**，
不得用 `generic_string()`（后者按本地代码页，中文名会变成 GBK 字节流，与 UTF-8 契约相抵）。
3. **进程入口用 `ST_MAIN(fn)`**（`st/core/entry.hpp`）：Windows 的 `argv` 是 ANSI
   （中文机器是 GBK），该宏在入口处转 UTF-8 并设好控制台代码页。
4. **类型与格式化**：需要定宽就用 `std::int32_t/int64_t/std::size_t`，**不用 `long` 表示字节数**；
   打印一律 `std::format`/`st::print`。
5. **系统库按目标平台解析**（`default_system_libs(platform)`）而非宿主宏：
   linux=`pthread dl m`、windows=`ws2_32`、darwin=按需声明。
   交叉编译在 `st.pkg` 的 `toolchains` 段声明，用 `st build <target> --toolchain=<名>` 启用。
6. **改了平台分支必须真的编一次**：交叉编译是唯一能发现 Windows 分支问题的途径
（实测：从未被编译过的 Windows 分支攒了 `nodiscard` 忽略返回值、未使用变量等一批 `-Werror` 问题）。
7. **窗口装饰一律自绘，任何平台都不得使用系统标题栏/边框**：平台后端的职责只有三件——把客户区
   像素贴上去、把输入翻译成 `ui::Event`、提供窗口控制（最小化/最大化/关闭/拖拽/边缘缩放），
   **不得**依赖窗口系统画标题栏或边框。落地方式（**已落地，可供其它平台照抄**）：
   ✅ 契约（`shell::Backend` 窗口控制接口 + `ui::WindowEdge`/`resize_edge_at` 判定）、
   ✅ 组件（`ui::TitleBar`）、✅ 装配（`st::app::Application` 同时实现 `control::Host` 与
   `ui::WindowControl`）、✅ Win32 落地（去装饰建窗 + `WM_NCCALCSIZE`/`WM_NCHITTEST` 接管）。
   理由有两条，都是硬的：
   ① **一致性**——三平台自带标题栏的字号/高度/圆角/配色各不相同，"一块代码三平台外观一致"
   这个承诺会从窗框处漏掉，且本项目 API 里根本没有系统标题栏的样式控制权；
   ② **可控性**——自绘窗框才可能与 UI 共用同一套设计令牌、同一套 DPI 口径与**同一份截图**
   （无头确定性截图是开发闭环的前提，而系统标题栏在无头下不存在，两侧画面就不可比）。
   需要平台能力时（去边框建窗、`WM_NCCALCSIZE`/`WM_NCHITTEST` 接管拖拽与八向缩放、
   最小化/最大化/关闭）**加在 `st::shell` 契约里**，由 `platform_*` 实现——应用层不得为此
   自行 `#if defined(_WIN32)` 写一套（回到第 1 条）。x11/wayland 补后端时只需实现那几个
   接口（含 `ui::resize_edge_at` 的边缘命中），**组件与应用一行不用改**。

### 10.1 编译器口径（两族：GCC 与 Clang，**同一条口径**）

**探测优先序**：`ST_CXX/CXX 显式指定 → g++（主版本 ≥ 13）→ clang++/c++`。
低于 13 的 g++ 缺 C++20 关键项（`<format>` 等），会被跳过并告警。

**为什么两族能共用一套构建命令**：清单恒写 GCC 风格（`-std=c++20`/`-Ifoo`/`-DNAME`/`-Wall`），
依赖产出恒用 `-MMD -MF`（写 GCC 风格 `.d`），链接恒用 `-lfoo`，运行库恒取 libstdc++。
因此本框架**没有"标志翻译层"**：清单写什么，命令行就是什么。这消掉了一整类问题
（"哪个平台漏了哪个开关"、"翻译后的标志与缓存键不一致"）。

**唯一需要显式声明的是目标三元组**：`clang++` 的默认目标跟着**它自己的构建方式**走——
LLVM 官方 Windows 包编译成 MSVC 目标（会去要 Visual Studio 的头与库）。要让 clang 走
"GCC 风格驱动 + libstdc++"这条口径，清单里给它 `"target_triple": "x86_64-w64-windows-gnu"`。
`g++` 不需要（默认目标就是本机）。该字段同时服务交叉编译（`i686-w64-mingw32` 等）。

**Windows 目标的链接默认静态 libgcc/libstdc++**（`-static-libgcc -static-libstdc++`，
两族都认）：否则产物要求 `libgcc_s_seh-1.dll`/`libstdc++-6.dll` 在 PATH 上。
san 档需要 sanitizer 运行库，MinGW 发行版多数不带——构建前会探测并给出可行动的报错。

**跨编译器检查是常规手段**：`st check --toolchain=clang` 只做语义分析（`-fsyntax-only`），
不产出、不链接，全量 77 单元约 15 s。它与 `build` 走同一段单元枚举与标志组装
（`BuildOptions::check_only`），所以"检查过了"就是"编得过"——
而**它查不出的东西要另外补**：链接期问题（ODR 违反、符号缺失、ABI 不匹配）只有真链接才暴露，
所以 `st check` 不能替代 `st build --toolchain=clang`，两者是"高频便宜"与"低频完整"的分工。
详见 `docs/BUILD_CHECK.md`。

**clang 是第二编译器（不替默认档）**。实测对比见 `docs/BUILD_TEST_PERF.md`：
不用 PCH 时 clang 快 7%，而**日常构建（带 PCH）gcc 快 15%**——所以默认仍是 g++。
但 clang 必须保留为**可选门禁**：它拓出过多处 GCC 看不到的自家代码缺陷
（死函数、未使用的 lambda 捕获、未使用的私有字段、`size_type→difference_type` 隐式变号、
系统头里的变号转换），这类差异在 GCC 上永远不会报。工具链专属的告警收敛写在清单的
`toolchains.<名>.suppressions`（**只用于 vendored 第三方头**，不往工程级 `flags` 里写——
否则等于给所有编译器统一放宽，把真检查一起丢掉）。

| 事项 | 规定 |
|---|---|
| 字符集 | 两族都按 UTF-8 读源文件，无需开关（源码一律 UTF-8，见 §1） |
| 依赖输出 | 恒 `-MMD -MF <文件>`（不解析任何"编译器专属的依赖格式"） |
| 系统库 | `pthread/dl/m` 在 `windows` 目标上自动剔除，`ws2_32/user32/gdi32/shell32` 反之；名单是**显式的**（`starts_with("win")` 那种前缀猜法会漏掉 `ws2_32` 这类主力库） |
| 目标不一致 | 编译与链接必须用**同一份** `--target`（`ResolvedToolchain::target_args()` 单点提供，两侧各写一遍是最难查的一类错） |

### 10.2 语言层面的行为差异（比平台 API 更阴魂）

有一类跨平台问题与系统 API 无关，而是**语言本身未规定**的地方在两个编译器上落地不同。
它们不会报错、不会告警，只在某一个平台上悄悄产出错误结果——**本机根本测不出来**：

| 事项 | 规定 |
|---|---|
| **函数实参求值顺序** | **不得在同一表达式里多次调用有状态函数**，如 `f(读游标(), 读游标())`。C++ **不规定**实参求值顺序——不同编译器、不同优化档都可能不一样。实测：图标路径解析因此把 x/y 交换，**所有 72 个图标都画错方向**，而在另一个编译器上完全正常；而它又不报错（"对钩交换后仍像对钩"）。回归：`tests/ui_icon_path_test.cpp` |
| 表达式内的副作用 | 同类问题：`a[i++] = i`、`f(x, ++x)`。拆成独立语句——多一行比一个只在奇偶编译器上出现的 bug 便宜得多 |
| 位域/结构体布局 | 跨协议序列化的结构体不用位域；字段顺序即布局约定（§6.5） |
| 字符与大小写 | `std::string::npos`、UTF-8 处理一律走 `st::str_*`；不用 `tolower`（与地区相关） |

**判据**：一行里出现两次同一游标/累加器的调用，就拆开。这类缺陷的唯一防线是“把结果拿出来对比”
（而不是“看起来能跑”）。

### 10.3 构建系统的三个**静默失效**点（前两条已用测试钉住）

1. **头文件依赖只能来自编译器**，不能靠“猜”。MSVC 走 `/sourceDependencies` JSON（见 §10.1）。
依赖清单出错**不会让构建失败**：它只会变短。短到只剩源文件时，改头文件不再触发重编，
而 `.o` 还是旧的——于是结构体改了成员、依赖它的单元却按旧布局编，**运行期以随机崩溃收场**
（实测：给 `Canvas` 加一个成员，界面起来就 access violation）。因此：
`depfile_from_source_dependencies` 有专测试（v1.2 扁平 + 早期嵌套 + 带空格路径 + 坏 JSON），
**任何依赖产出方式的改动都要同时改测试**。
2. **共享对象缓存的键必须包含“依赖产出格式版本”**（`<deps:N>`）。缓存项的有效性靠它随身的 `.d` 判定，
格式一改，旧条目的 `.d` 就可能不完整，而“缓存命中”会把这个错误永久钉住。
3. **PCH 会掩盖缺失的标准库头**——`st build` 编得过，`bootstrap` 编不过。
`st build` 自动 `-include st/pch.hpp`，而 PCH 里含整套常用标准库头（`<map>`/`<format>`/`<ranges>`…），
于是某个 `.cpp` 漏写 `#include <map>` 时**日常构建完全看不出来**；直到首次自举才炸——
`bootstrap.sh`/`bootstrap.ps1` 跑的正是「还没有 PCH」的那一次编译，报错形态是
`error: 'map' is not a member of 'std'`，且行号指向**使用处**（`stats.cpp:372`）而非缺失处。
实测：`src/pkg/stats.cpp` 用了 4 处 `std::map` 却从未 `#include <map>`（`stats.hpp` 也不传递引入它），
缺陷自 `st stats` 引入起就潜伏——**所有含该文件的 `st build` 都是绿的**，只有一个全新环境自举才暴露。
防线是 §6 第 6 条（翻译单元自包含）：**PCH 是加速手段，不是契约**，编译器报错的前提是先能看见声明。
新增标准库用法后自查一句：「把这个 `.cpp` 单独丢给 `g++ -fsyntax-only`（不带 `-include`）还编得过吗」。
改完一批后跑全量自查（先 `st build` 一次，以生成 `build/dev/embed/lib/include` 下的 `battery/embed.hpp`）：

```bash
cd build/..   # 工程根
FLAGS='-std=c++20 -fsyntax-only -DST_VERSION="0.1.0" -DST_ENABLE_LINT=1 \
  -Iinclude -Ithird_party -Ithird_party/sqlite -Ibuild/dev/embed/lib/include'
for f in $(ls src/*/*.cpp src/*/*/*.cpp); do g++ $FLAGS "$f" >/dev/null || echo "FAIL $f"; done
```

实测（2026-10）：本约定落地时全量 89 个翻译单元零报错零警告；把 `stats.cpp` 的 `<map>` 回退后
同一命令报 18 个错——即这条命令对该缺陷确有判别力，不是一句空口号。

> Windows 回退：刚写完的文件可能被索引/杀毒进程短暂占用，`fs::rename` 会以
> “另一个进程正在使用此文件”失败（并行编译下偶发地把整次构建弄挂）——已在
> `fs::rename` 里对 `ERROR_SHARING_VIOLATION`/`ERROR_ACCESS_DENIED` 做短暂重试。
