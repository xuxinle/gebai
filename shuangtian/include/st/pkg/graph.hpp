#pragma once

/// 包级依赖图（对应 Go 的 import 图）：支撑「按需编译」。
///
/// ## 为什么要有它
///
/// 现在的构建是**全量编译 + 链接期裁剪**：库级 233 个框架单元全部编进归档，
/// 再由链接器按引用拉取。编译成本因此与应用实际用量无关——哪怕一个应用
/// 完全不碰 3D / LSP / 打包器，它仍然要先把这些源编成对象。
///
/// Go 的做法相反：编译器掌握**精确的 import 图**，没被 import 的包根本不编译。
/// 本模块把那张图在霜天重建出来，于是"编什么"可以按需推导，而不是恒定全量。
///
/// ## 图是怎么算出来的
///
/// 从 `#include` 关系推导，**头文件与实现两侧都算**（严口径）：
///
/// - 只看头文件（松口径）会漏掉"实现依赖但头文件不暴露"的边
///   （`src/net/platform_tls_*.cpp` 这类 `platform_*` 单元是典型）；
/// - 漏边的后果是**裁掉一个实际被需要的包**，症状是链接期 undefined reference。
///
/// 所以闭包必须按严口径算。
///
/// ## 包的归属规则（关键，踩过坑）
///
/// **不能用"源码目录 → 包名"直接映射**。实测反例：`src/geom/*.cpp` 里
/// 13 个单元中有 11 个实现的是 `st/raster/*.hpp`（`scene_graph` / `gltf` / `skin` …），
/// 只有 `animation.cpp` 另有归属。按目录规则会把它们判成 `st/geom` 包，
/// 而 `st/geom` 对多数应用"不需要"——于是 **gallery 会裁掉它其实必需的
/// `scene_graph.cpp`**，链接才报错。
///
/// 规则因此是**同名头优先**：`src/a/b/x.cpp` 若存在头 `st/a/b/x.hpp`，归 `st/a/b`；
/// 找不到同名头才回退到"目录同名"（`src/core/platform_*.cpp` 这类没有公开头的单元）。
///
/// ## 循环依赖：按强连通分量处理，而不是要求零环
///
/// Go 硬性禁止包循环。霜天**做不到**，也不该为此重排代码——实测存在一处：
/// `st/ui` ↔ `st/ui/components`（`ui_root.cpp` 要用 `feedback`/`panel` 组件，
/// 组件又要用 `theme`/`icon`/`text_port`）。
///
/// 处理方式与 Go 的包图并不冲突：**把强连通分量收缩成一个节点**再拓扑。
/// 分量内部本来就无法互相裁剪（它们必须同生共死），收缩后得到 DAG，
/// 闭包与按需编译照常成立。这也是 `go build` 之外多数构建系统的做法。
///
/// ## 自注册：一类**推不出来**的隐式依赖
///
/// 框架里有 58 个单元靠静态初始化自注册（`ST_UI_COMPONENT_IMPL` →
/// `component_registry::add`）。这类依赖不进 include 图：声明式按名创建一个组件时，
/// 代码里既没有 include 也没有符号引用，全靠"那个 `.o` 恰好在产物里"。
///
/// 实测结论：这 58 个**全部**落在 `st/ui` / `st/ui/components`，而这两个包
/// 属于同一个强连通分量、且是所有 UI 应用必经依赖——所以按需裁剪**不会**误伤它们。
/// 但这条结论必须被钉住（见 `pkg_graph_test`），否则将来新增一个自注册包
/// 到可裁位置，症状会是"运行期按名创建失败"，而不是编译错误。

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::pkg {

/// 一个包：名字 + 它包含的头文件与实现单元（都是**相对仓库根**的 posix 路径）。
struct Package {
  std::string name{};                  ///< 如 `st/ui/components`
  std::vector<std::string> headers{};  ///< 该包的头文件
  std::vector<std::string> units{};    ///< 该包的实现单元（`.cpp`）
};

/// 包级依赖图。
///
/// 构造后是**不可变**的：`packages()` / `dependencies()` 只读，
/// 闭包与裁剪查询都基于它算，不修改图本身。
class PackageGraph {
 public:
  /// 从文件集合构图。
  ///
  /// - `headers`：头文件路径（相对仓库根，如 `include/st/ui/button.hpp`）
  /// - `units`：实现单元路径（相对仓库根，如 `src/ui/button.cpp`）
  /// - `read_includes`：给一个文件路径，返回它 `#include "st/…"` 的那些头（相对 `include/`）。
  ///   注入而不是内置读文件，理由：本模块要能被单元测试用**纯内存**数据驱动，
  ///   也要能被构建器用真实文件系统驱动，两处共用同一段图算法。
  using IncludeReader = std::function<std::set<std::string>(const std::string&)>;

  [[nodiscard]] static auto build(const std::vector<std::string>& headers,
                                 const std::vector<std::string>& units,
                                 const IncludeReader& read_includes) -> PackageGraph;

  /// 全部包（按名字排序，便于稳定输出与测试）。
  [[nodiscard]] auto packages() const -> const std::vector<Package>& { return packages_; }

  /// 包名 → 它直接依赖的包名集合（已收缩强连通分量内部的自环）。
  [[nodiscard]] auto dependencies() const -> const std::map<std::string, std::set<std::string>>& {
    return dependencies_;
  }

  /// 强连通分量（每个分量是包名列表）。分量数 ≥ 1，多包分量即"循环依赖"。
  [[nodiscard]] auto components() const -> const std::vector<std::vector<std::string>>& {
    return components_;
  }

  /// 包名 → 所属分量下标。
  [[nodiscard]] auto component_of(const std::string& package) const -> std::optional<std::size_t>;

  /// 从 `roots` 出发的**传递闭包**（含 roots 自身）。
  ///
  /// 按分量算：一个分量内任一包可达，整个分量都算可达——它们本来就无法分开编译。
  [[nodiscard]] auto closure(const std::set<std::string>& roots) const -> std::set<std::string>;

  /// 该闭包内需要保留的实现单元（排序后）。
  [[nodiscard]] auto units_for(const std::set<std::string>& packages) const -> std::vector<std::string>;

  /// 无辜的环检测：返回所有"多包分量"（大小 > 1），空表示图本是 DAG。
  [[nodiscard]] auto cycles() const -> std::vector<std::vector<std::string>>;

  /// 图上所有边的总数（诊断用）。
  [[nodiscard]] auto edge_count() const -> std::size_t;

 private:
  std::vector<Package> packages_{};
  std::map<std::string, std::set<std::string>> dependencies_{};
  std::vector<std::vector<std::string>> components_{};
  std::map<std::string, std::size_t> component_index_{};
};

/// 头文件路径 → 所属包名（如 `st/ui/components/button.hpp` → `st/ui/components`）。
///
/// 输入是**相对 `include/`** 的路径（`st/…` 开头）。取到目录层，去掉文件名。
[[nodiscard]] auto package_of_header(std::string_view relative_to_include) -> std::string;

/// 实现单元路径 → 所属包名。
///
/// `src/a/b/x.cpp` → `st/a/b`（常规情况）。**同名头优先**，四个判据按强弱递减：
///
/// 1. **单元自己 include 了哪个同名头**（最权威）——`src/geom/animation.cpp`
///    第一行就是 `#include "st/raster/animation.hpp"`，哪怕 `st/ui/animation.hpp`
///    这个同名头同时存在，也能确定它归 `st/raster`；
/// 2. 同目录同名头存在 → 就是这个包（`src/ui/button.cpp` → `st/ui`）；
/// 3. 全局唯一同名头 → 用它的目录（`src/geom/gltf.cpp` → `st/raster`）；
/// 4. 都判不了 → 回退目录同名（`platform_*` 那类无公开头的单元）。
///
/// 为何不能只用路径（实测反例）：`src/geom/` 的 13 个单元里 11 个实现的是
/// `st/raster/*.hpp`，按目录判会集体错归 `st/geom`，于是“不需要 st/geom 的应用”
/// 会把这些单元全部裁掉，链接期报 `undefined symbol`（jianmu / gallery 都实测触发过）。
///
/// `header_lookup(stem)` 返回**所有**基名相同的头路径（相对 `include/`，`st/…` 开头）。
/// `std::nullopt` = “未登记基名”（不参与同名头规则，直接回退目录）。
///
/// `self_includes`（可选）给出该单元自己的 `st/…` include 列表——即判据 1 的输入源；
/// 传 `nullptr` 则不使用判据 1（便于调用方只要路径推断时用）。
using HeaderLookup = std::function<std::optional<std::vector<std::string>>(std::string_view stem)>;
using IncludeReader = std::function<std::set<std::string>(const std::string&)>;
[[nodiscard]] auto package_of_unit(std::string_view relative_to_repo, const HeaderLookup& header_lookup)
    -> std::string;
[[nodiscard]] auto package_of_unit(std::string_view relative_to_repo, const HeaderLookup& header_lookup,
                                  const IncludeReader* self_includes) -> std::string;

}  // namespace st::pkg
