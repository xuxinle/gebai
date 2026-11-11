#pragma once

/// `st.pkg` 清单（JSON，`DESIGN.md` §7.2）：模块划分、源文件 glob、构建目标与三类依赖。
/// 约定：
/// - `directory` 是**运行时上下文**（清单所在目录的绝对路径），用于展开 glob 与解析相对源地址，
///   不作为清单字段写出；
/// - `sources` / `tests` 支持 `st::fs::expand_glob` 通配（`*`、`?`、`**`），展开结果为绝对路径；
/// - 依赖分三类：`dependencies`（第三方源码，多来源）、`dependency_modules`（同仓模块）、
///   `dependency_system`（系统链接项：`dl`/`pthread`/`m`，非第三方源码）；
/// - `extra_fields` 保存本层未识别的顶层字段，`to_json` 原样回写（与真实清单互操作时不丢字段）。

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/error.hpp"
// ⚠ 这里**必须**是完整头（不是前向头）：`Manifest` 的成员 `extra_fields` 是 `st::Json`，
// 而**类成员不能是不完整类型**（语言限制，`empty_json_object()` 也救不了）。
// 代价：任何包含本头的编译单元都要真解析 nlohmann（约 13.8 万预处理行）。
// 实测结论见 `st/core/json_fwd.hpp` 的表格。
#include "st/core/json.hpp"

namespace st::pkg {

/// 依赖来源规格（`Path` 本地目录 / `Http` 明文 tar.gz / `GitTarball` 本地归档或已解包目录 /
/// `Registry` 索引地址：`file://` 或 `http://`）。
struct SourceSpec {
  enum class Kind : std::uint8_t { Path, Http, GitTarball, Registry };

  Kind kind{Kind::Path};
  std::string location{};  ///< Path: 本地目录；Http: URL；GitTarball: 本地归档/目录；Registry: 索引地址
  std::string sha256{};    ///< 期望校验和（空 = 不校验；目录源按 `st::hash::tree_fingerprint` 语义）

  /// 是否为本地目录形态的来源（`Path`，或 `GitTarball` 指向已解包目录）。
  [[nodiscard]] auto is_directory_source() const noexcept -> bool {
    return kind == Kind::Path || kind == Kind::GitTarball;
  }
};

/// 来源类型的稳定短名（清单/锁文件/日志序列化用）。
[[nodiscard]] auto source_kind_name(SourceSpec::Kind kind) -> std::string_view;
/// 解析来源类型短名；未知名称返回 `Parse`。
[[nodiscard]] auto parse_source_kind(std::string_view text) -> Result<SourceSpec::Kind>;
/// 来源规格 → JSON 对象（`{"kind","location","sha256"}`）。
[[nodiscard]] auto source_to_json(const SourceSpec& spec) -> st::Json;
/// 从对象解析来源规格：支持 `{"kind","location"|"url","sha256"}` 平铺形式，
/// 以及 `{"source":{...},"sha256":"..."}` 嵌套形式（嵌套缺 `sha256` 时取外层）。
[[nodiscard]] auto source_spec_from_object(const st::Json& json) -> Result<SourceSpec>;

/// 第三方源码依赖项。
struct DependencySpec {
  std::string name{};
  std::string version_req{"*"};  ///< 版本约束文本（见 `st::pkg::VersionReq`；前缀 `?` 表示可选依赖）
  SourceSpec source{};
};

/// 构建目标（可执行/库）。
struct TargetSpec {
  std::string name{};
  std::string kind{"executable"};
  std::vector<std::string> sources{};
  std::vector<std::string> flags{};
  /// 从库源中排除的 glob（如工具链专属的 `src/pkg/*`——应用不需要，排除后少编译若干翻译单元）。
  std::vector<std::string> exclude_sources{};
  /// **白名单**：本目标只编译这些 glob（而不是「顶层全部源减去黑名单」）。
  ///
  /// ## 为何需要它（黑名单失效时是静默的）
  ///
  /// `exclude_sources` 是黑名单：漏掉一条，就多编几个无关单元，而构建照常成功、
  /// 看不出任何异常——只能靠人去把每条间接依赖都推一遍。实测代价：`st` 目标
  /// 逐项排了 13 条，仍因 `src/ui/window_control.cpp` 间接依赖 `math/geometry.hpp`
  /// 把 math 拉了回来。
  ///
  /// 白名单相反：**漏写会链接报错**（可发现），不会静默变胖。
  /// 语义：非空时只编这些 glob，`exclude_sources` 不再参与。
  std::vector<std::string> include_sources{};
  /// **第三方源白名单**：本目标只编这些 glob 命中的第三方源（`third_party_sources` 的目标级裁剪）。
  ///
  /// ## 为什么需要它（工程级全量的两个实测代价）
  ///
  /// `third_party_sources` 是工程级的：一家进清单，**所有**目标都编它。对走按需链接
  /// （归档 + gc-sections）的目标，多余的第三方只是白编几个单元；但对**直铺 .o** 的
  /// 链接路径（自举脚本 `bootstrap.ps1`/`bootstrap.sh`），没人引用的第三方成员会被
  /// 强制全链——而它们引用的移植层符号在（被 `include_sources` 正确滤掉的）框架源里，
  /// 链接期报一串 `undefined reference`。实测：引入 `third_party/micropython/py/*.c`
  /// 后自举立即可复现（`gc_collect`/`mp_hal_stdout_tx_strn_cooked`/`nlr_jump_fail`）。
  ///
  /// 语义与 `include_sources` 同哲学：白名单漏写 → 链接报错（可发现），不会静默变胖；
  /// 缺省/空 → 维持工程级全量（既有目标零改动）。
  std::vector<std::string> include_third_party_sources{};
  /// 编译期嵌入的资源文件 glob（`battery/embed.hpp`，见 `third_party/battery/UPSTREAM.md`）。
  ///
  /// 写在**目标**上而不是工程级：嵌入集合决定生成的声明头内容，而声明头是按目标隔离的
  /// （不同目标嵌入不同资源，且同名资源在不同目标里标识符不同）。
  std::vector<std::string> embed{};
  /// **可选**嵌入的资源（语义同 `embed`，但**缺失不报错**）。
  ///
  /// 用途：由本地工具产出、因体积不入库的大资源（例：格斗的 36 张 AI 生图立绘 ~5.4MB，
  /// `.gitignore` 有全局 `*.png`）。这类文件在产线机器上存在、在克隆机器上必然不存在；
  /// 对它按「路径写错」报错等于把「没素材」当成「清单写错」，
  /// 而且整个应用连**编译**都过不了（它本可降级为矢量绘制跑起来）。
  ///
  /// 构建行为：未命中的字面路径生成**零字节占位**（`b::embed<"…">()` 仍能编译），
  /// 运行时解码失败 → 调用方已有的降级路径生效；构建日志会点名缺了哪些文件。
  ///
  /// 为何另起一个字段而不给 `embed` 加语法前缀：路径本身就是**查找键字面量**
  /// （`b::embed<"apps/x/a.png">()` 按它查表），往里塞标记会把键弄脏。
  /// 带通配符的模式（`*.png`）在缺失时无字面键可补，只记日志跳过——
  /// 需要降级的资源请**逐文件写出**。
  std::vector<std::string> optional_embed{};
   /// 日志代码是否编进本目标（覆盖工程级；`std::optional` 才能区分“未指定”与“显式 false”）。
 std::optional<bool> log_enabled{};
 /// 目标专属编译宏（裸名，如 `ST_NO_SCRIPT=1`；构建侧注入 `-D`）。
 ///
 /// 与交叉工具链 `defines` 的区别：那个作用于**整条工具链**（所有目标），
 /// 这个只作用于**本目标**的单元——典型用途是按需链接的拆链开关
 /// （如 gbcode 定义 `ST_NO_SCRIPT` 后 QuickJS 整链不进产物）。
 std::vector<std::string> defines{};
  /// 链接子系统：`"console"`（默认）或 `"gui"`。
  ///
  /// 仅 Windows 有区别：`gui` 对应链接器的 `-mwindows`（PE 头的 Subsystem=2
  /// WINDOWS_GUI），双击 exe **不弹控制台窗口**；`console` 是 Subsystem=3，
  /// 双击会先冒一个黑框。GUI 应用发布时必须用 `gui`。
  ///
  /// 为何不给所有目标默认 `gui`：工具链（`st`）、探测程序、e2e 辅助工具都要
  /// 在终端里跑并读输出，转成 GUI 会让 `stdout` 无处可写。默认 `console`
  /// = 保持既有行为不变，需要图形界面的**应用**显式声明。
  ///
  /// 非 Windows 平台忽略本字段（没有子系统概念）。
  std::string system{"console"};
  /// 本目标**依赖的库目标名**（`kind = "library"` 的目标有意义，且只能引用同工程内的）。
  ///
  /// **`kind = "group"` 时语义不同**：这里是**成员目标名**（依次构建哪些目标），
  /// 不限于库——典型就是 `"libraries": ["gallery", "gbcode", ...]` 这种应用集。
  /// 同一个字段两种含义是有意为之：它们描述的是同一件事（“本目标依赖哪些其它目标”），
  /// 只是 group 对它们是“构建”而不是“链接”。
  ///
  /// ## 为何要这个字段（而不是自动推断）
  ///
  /// 可执行目标原本靠 `sources/exclude_sources/include_sources` 自己拉源，从不「链库」。
  /// 要支持真正的库（`kind = "library"`），就得有个地方声明依赖关系，否则：
  /// · 编库时不知道产物叫什么；
  /// · 编可执行时不知道要链哪些 `.a`。
  ///
  /// 不做**自动推断**（扫源码里的 include 猜依赖）的理由：那是启发式，猜错时
  /// 要么漏链（链接报错，尚可发现）要么多链（产物变大，静默）。显式声明让
  /// “谁依赖谁”成为清单里的**事实**而不是推断。
  ///
  /// group 同理：**不按“所有 executable”自动枚举**——那会把 140 个一次性探针
  /// 也带上（`tools/*_probe.cpp`），而“构建全部应用”不该等于“构建全部探针”。
  std::vector<std::string> libraries{};
  /// 本目标是否接收**工程级嵌入**（缺省 true；`kind = "library"` 的库通常应关）。
  ///
  /// ## 为何需要它（实测踩到的事）
  ///
  /// 工程级 `embed`（如本仓的 `src/ui/script_api.js`）原本会**无条件**并入每个目标。
  /// 于是 `st_core`（通用库）里会多出两个 UI 的 JS 字节数组成员——它们既不属于
  /// “通用层”，又让库被动依赖了 UI 的构建产物。
  ///
  /// 这不是“体积大一点”的问题：库的语义是“不依赖 UI 的可复用部分”，
  /// 而嵌入物把 UI 的产物也拉进来了——**声明与实现不一致**。
  ///
  /// 故给一个显式开关，默认 `true` 保持既有行为（应用目标不需要改清单）。
  std::optional<bool> inherit_project_embed{};
  /// Windows 可执行文件的**图标**（`.ico` 路径；可选）。
  ///
  /// 为何不能只靠运行期 `SetWindowIcon`：那换的是**窗口**图标，而资源管理器/
  /// 任务栏/快捷方式看到的是 **PE 文件里的图标资源**（`RT_GROUP_ICON`）。
  /// 没有那份资源，exe 在资源管理器里就是空白默认图标。
  /// 做法是把 `.ico` 编成 `.o` 再链进产物（见 `build.cpp` 的 `compile_resource`）。
  std::string icon{};
};

/// 交叉编译工具链描述（`st.pkg` 的 `toolchains` 段）。
///
/// 存在的理由：交叉编译时**宿主与目标平台不同**，而"默认系统库/可执行文件后缀/平台宏"
/// 这些必须按**目标**决定，不能按宿主（`#if defined(_WIN32)` 判的是宿主，必然错）。
struct ToolchainSpec {
  std::string name{};
  std::string compiler{};      ///< C++ 编译器命令（如 `x86_64-w64-mingw32-g++`）
  std::string c_compiler{};    ///< C 编译器（空则取 compiler 并加 `-x c`）
  std::string platform{};      ///< 目标平台：`windows` / `linux` / `darwin` / `none`
  std::vector<std::string> system_libs{};   ///< 目标平台额外系统库（如 `ws2_32`）
  std::vector<std::string> defines{};       ///< 目标专属宏（如 `_WIN32_WINNT=0x0601`）
  std::string executable_suffix{};          ///< 产物后缀（Windows 为 `.exe`）
  std::vector<std::string> extra_flags{};   ///< 目标专属编译/链接标志
  /// 目标专属**告警收敛**（追加在工程严格集之后）。
  ///
  /// 用途：同一份源码在**所有**编译器上与 GCC 同等严格是目标，但在做到之前，
  /// 某族编译器会对第三方头发出工程无法修复的告警（实测：clang 的 `-Wpedantic`
  /// 从 `sqlite3.h` 的 `__int64`、`quickjs.h` 的旧式转换里报错，而那是 vendored
  /// 上游代码、不是我们要改的东西）。这类差异写在这里，**不写进工程级 flags**——
  /// 否则等于给所有编译器统一放宽，把真检查一起丢掉。
  ///
  /// 与 `extra_flags` 分开的理由：`extra_flags` 参与链接，这里只影响编译；
  /// 语义上它是"对某族编译器的例外"，不是目标平台属性。
  std::vector<std::string> suppressions{};
  /// 目标三元组（传给编译器与链接器的 `--target=`；空 = 用编译器自己的默认目标）。
  ///
  /// 为何需要在清单里显式声明：`clang++` 的默认目标跟着**它自己的构建方式**走——
  /// LLVM 官方 Windows 包编译成 MSVC 目标（吃 Visual Studio 的头与库）。想让 clang 走
  /// “GCC 风格驱动 + libstdc++”那条口径（不依赖 VS），就写
  /// `"target_triple": "x86_64-w64-windows-gnu"`。g++ 不需要它（默认目标就是本机）。
  ///
  /// 同一字段也服务交叉编译：`i686-w64-mingw32`、`aarch64-linux-gnu` 都写在这里
  /// （交叉编译器的名字本身已带前缀时留空即可）。
  std::string target_triple{};
};

/// 独立工程对**骨天框架**的引用（`st.pkg` 的 `framework` 段）。
///
/// 为什么是"指向框架根目录"而不是"预编译库 + 安装步骤"：
/// 安装位置、ABI/编译器版本、交叉编译两套产物都是持续的麻烦；而源码级引用
/// **无需安装、始终同版本、离线可构建、交叉编译天然生效**。代价（每个工程首次要编框架源）
/// 由对象缓存摊掉（见 `pkg/cache.hpp`）。
struct FrameworkSpec {
  /// 框架根目录（含 `st.pkg`）。支持清单里的相对路径（相对清单目录）。
  std::string path{};
  /// 解析后的绝对路径（`Manifest::resolve_directories` 后填充）。
  std::string directory{};
  /// 是否连框架的编译选项（严格告警集/宏）一起继承。默认继承：
  /// 独立工程与框架共用同一套质量线，避免"在框架里干净、在工程里漏报"。
  bool inherit_flags{true};
};

/// `st.pkg` 清单。
struct Manifest {
  std::string name{};
  std::string version{"0.1.0"};
  std::string kind{"static_library"};
  /// C++ 语言标准（`-std=c++XX` 的年号值：20 / 23）。**构建侧唯一的真源**：
  /// 构建驱动、PCH 创建与消费、自举脚本都从这里派生，不再各自硬编码串。
  ///
  /// 缺省取 `kDefaultCxxStandard`。年号是否"能编"由构建侧判断（框架源码不保证
  /// 20 以下可编），本字段只承载清单声明。
  static constexpr std::uint32_t kDefaultCxxStandard = 23;
  /// 标准年号 → 编译器标志（`-std=c++23`）。接受域 `20..26` 之外返回空串。
  [[nodiscard]] static auto cxx_standard_flag(std::uint32_t standard) -> std::string;
  std::uint32_t cxx_standard{kDefaultCxxStandard};
  std::string directory{};  ///< 清单所在目录（绝对路径，供源/包含路径解析）
  std::vector<std::string> modules{};
  std::vector<std::string> include_dirs{};
  std::vector<std::string> sources{};  ///< 支持 glob（`src/**/*.cpp`）
  /// 第三方源码 glob（`third_party/**`）：这些翻译单元**不套用本工程的严格告警集**（`-w`），也不进 PCH。
  /// 理由：第三方码不是我们的代码，`-Werror` 会让"升级上游"变成"改上游"——违背上游可追溯原则。
  std::vector<std::string> third_party_sources{};
  std::vector<std::string> tests{};
  std::vector<std::string> flags{};
  /// 工程级编译期嵌入（随**库**编译，对所有目标可见）。
  ///
  /// 与目标级 `TargetSpec::embed` 的分工：库级用于"框架自身要用的资源"
  /// （如脚本运行时前置 `src/ui/script_api.js`——它属于框架实现，不该让每个应用重复嵌入）；
  /// 目标级用于"应用自己的资源"。标识符前缀分别取工程名与目标名。
  std::vector<std::string> embed{};
  /// C 源（`.c`）专用标志（默认 `-std=gnu11`）；C 源不得用 `-std=c++XX` 编。
  std::vector<std::string> c_flags{};
  std::vector<std::string> defines{};
  std::vector<std::string> system_libs{};
  /// 日志代码是否编进产物（`"log": false` 裁掉；缺省真）。目标级同名键可覆盖单个目标。
  ///
  /// 构建侧据此加 `-DST_LOG_DISABLED=1`（见 `st/core/log.hpp`）——**编译期**裁掉，
  /// 不是“运行时判断一次再跳过”（后者代码与字符串仍在产物里）。
  bool log_enabled{true};
  std::vector<TargetSpec> targets{};
  /// 交叉编译工具链（按名选取：`st build <target> --toolchain mingw`）。
  std::vector<ToolchainSpec> toolchains{};
  /// 引用的霜天框架（空 = 本工程即框架，或不需要框架）。
  std::optional<FrameworkSpec> framework{};
  std::vector<DependencySpec> dependencies{};     ///< 第三方源码依赖
  std::vector<std::string> dependency_modules{};  ///< dependencies.modules
  std::vector<std::string> dependency_system{};   ///< dependencies.system
  /// 未识别顶层字段（回写保留，互操作用）。
  ///
  /// 必须用 `=` 拷贝初始化：写成 `st::Json extra_fields{st::Json::object()}` 会命中
  /// nlohmann 的 initializer_list 构造，得到「含一个空对象的**数组**」而非对象，
  /// 之后按对象使用即抛 `type_error.305`——花括号在 nlohmann 里是「造数组」的信号。
  st::Json extra_fields = empty_json_object();
  /// 规则豁免（`lint.exempt`）：规则 id → 路径 glob 列表（相对工程根）。
  /// 用途与两条豁免通道的分工见 `st::pkg::LintExemptions` 与 `lint.cpp` 的 `manifest_exempt`。
  std::map<std::string, std::vector<std::string>> lint_exempt{};

  /// 查目标（不存在返回 nullptr；返回指针非拥有，生命周期同本清单）。
  [[nodiscard]] auto find_target(std::string_view target_name) const -> const TargetSpec*;
  /// 查工具链（不存在返回 nullptr）。
  [[nodiscard]] auto find_toolchain(std::string_view toolchain_name) const -> const ToolchainSpec*;
  /// 展开 `sources` 的 glob，返回绝对路径列表（按模式顺序 + 模式内字典序，跨模式去重）。
  /// 错误：`Invalid`（缺 `directory`）、`NotFound`（目录不存在）、`Io`。
  [[nodiscard]] auto source_files() const -> Result<std::vector<std::string>>;
  /// 展开 `tests` 的 glob（语义同 `source_files`）。
  [[nodiscard]] auto test_files() const -> Result<std::vector<std::string>>;
  /// 展开 `third_party_sources` 的 glob（第三方源码：放宽告警、不进 PCH）。
  [[nodiscard]] auto third_party_files() const -> Result<std::vector<std::string>>;
  /// 解析清单 JSON；`directory` 为清单所在目录（绝对路径）。错误：`Parse`。
  static auto parse_json(const st::Json& json, std::string_view directory) -> Result<Manifest>;
  /// 读取 `st.pkg` 文件（`path` 为文件路径）。错误：`NotFound`/`Io`/`Parse`。
  static auto load(std::string_view path) -> Result<Manifest>;
  /// 在目录里查找 `st.pkg` 并读取。错误：`NotFound`（无清单）/`Io`/`Parse`。
  static auto find(std::string_view directory) -> Result<Manifest>;
  /// 序列化为清单 JSON（含 `extra_fields`；不写出 `directory`）。
  [[nodiscard]] auto to_json() const -> st::Json;
  /// 写出清单文件（pretty JSON）。错误：`Io`。
  static auto write(std::string_view path, const Manifest& manifest) -> Status;
};

}  // namespace st::pkg
