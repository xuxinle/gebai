#pragma once

/// 文件系统与路径工具（`std::filesystem` 之上的薄封装，隔离平台差异）。
/// 约定：所有路径参数用 `std::string`（UTF-8），返回值统一为 `Result`/`std::optional`。
///
/// 只读内存映射声明在此处（`MappedFile`）、实现在 `src/core/platform_fs_map.cpp`——
/// 平台头只允许出现在那一层（`CONVENTIONS.md` §10 第 1 条 / lint L14）。

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/fs_map.hpp"

namespace st::fs {

using Bytes = std::vector<std::uint8_t>;

/// 只读**内存映射**文件（POSIX `mmap` / Windows `CreateFileMapping`）。
///
/// 为什么需要它（2026-10-14 内存评估的 P0）：「整读进 vector」是霜天内存占用的头号来源
/// ——字体链 15 个 face 实测 ~86MB 常驻且**永不释放**。映射与整读的差别不在「快慢」，而在
/// **内存归属**：映射进地址空间的是文件页的**共享副本**，系统在内存压力下可直接回收
/// （后备存储就是文件本身，无脏页），而 `read_bytes` 的私有堆副本只能靠换页。
///
/// 契约：
/// - **安全失败**：打开/取大小/映射任一步失败都返回 `Error`，调用方自行回退（不抛异常）。
/// - **空文件视为失败**：映射长度 0 在 POSIX 是 `EINVAL`，而"没有内容"与"读不到"
///   对调用方是同一件事（字体路径会因此回退到整读，拿到同样的空文件错误）。
/// - **只读**：映射是 `PROT_READ`/`FILE_MAP_READ`；写入会崩（这是**刻意**的——
///   解析层拿到的是 `span<const uint8_t>`，写不进）。
/// - **生命周期**：视图在对象存活期内始终有效（析构时解映射）；不可拷贝、可移动。
/// - ⚠ **文件被外部替换**（重装字体、构建覆盖产物）不会使已有映射失效，但映射内容仍是
///   旧文件——需要"读到新内容"的地方得重新 `open`（字体缓存为此按 (路径, face) 缓存，
///   而缓存的失效本来就是"进程重启"粒度）。
class MappedFile {
 public:
  MappedFile() = default;
  MappedFile(const MappedFile&) = delete;
  auto operator=(const MappedFile&) -> MappedFile& = delete;
  MappedFile(MappedFile&& other) noexcept;
  auto operator=(MappedFile&& other) noexcept -> MappedFile&;
  ~MappedFile();

  /// 只读映射整个文件；失败返回 `Error`（`NotFound` 打不开、`Io` 映射失败）。
  [[nodiscard]] static auto open(std::string_view path) -> Result<MappedFile>;

  /// 映射视图（非空映射的长度即文件大小）。
  [[nodiscard]] auto span() const noexcept -> std::span<const std::uint8_t>;
  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto valid() const noexcept -> bool { return data_ != nullptr; }

 private:
  /// 平台映射视图（句柄 + 数据指针；平台细节单点在 `platform_fs_map.cpp`）。
  MappedView view_{};
  const std::uint8_t* data_{nullptr};
  std::size_t size_{0};

  void reset() noexcept;
};

/// 读文件的一段（**只读需要的字节**）。
///
/// 与 `MappedFile` 的分工：映射适合"要整文件且要反复随机访问"（字体解析）；本函数适合
/// "只需头部"（判断格式/取 face 数）——不必把 20MB 的 `.ttc` 拉进地址空间只为读 12 字节。
/// 越出文件末尾的请求**截断到实际可读长度**（不报错）：调用方本来就按"够不够判别"用。
[[nodiscard]] auto read_span(std::string_view path, std::uint64_t offset, std::size_t length)
    -> Result<Bytes>;

struct DirEntry {
  std::string name{};
  std::string path{};  ///< 与入参同基准的路径
  bool is_dir{false};
  std::uint64_t size{0};
};

[[nodiscard]] auto read_text(std::string_view path) -> Result<std::string>;
[[nodiscard]] auto write_text(std::string_view path, std::string_view content) -> Status;
[[nodiscard]] auto append_text(std::string_view path, std::string_view content) -> Status;
[[nodiscard]] auto read_bytes(std::string_view path) -> Result<Bytes>;
[[nodiscard]] auto write_bytes(std::string_view path, std::span<const std::uint8_t> data) -> Status;

[[nodiscard]] auto exists(std::string_view path) noexcept -> bool;
[[nodiscard]] auto is_directory(std::string_view path) noexcept -> bool;
[[nodiscard]] auto is_regular_file(std::string_view path) noexcept -> bool;
[[nodiscard]] auto file_size(std::string_view path) -> Result<std::uint64_t>;
/// 最后修改时间（Unix 纪元纳秒；不存在返回错误）。
[[nodiscard]] auto modified_ns(std::string_view path) -> Result<std::int64_t>;
/// 把最后修改时间置为“现在”（等价 `touch`）。
///
/// 为何需要：`std::filesystem::copy_file` 在 Windows 上（经 `CopyFile`）
/// **保留源文件时间戳**。对象缓存的 restore 用它把缓存对象拷回来，于是
/// “内容已是新的、mtime 还是旧条目那时的”——任何依赖 mtime 的后续判断
/// （如“对象比产物新吗”）都会被误导。restore 后显式 touch 一次，
/// 让“.o 是刚构建过的”这个语义在时间戳上也成立。
[[nodiscard]] auto touch_now(std::string_view path) -> Status;

[[nodiscard]] auto list_dir(std::string_view path) -> Result<std::vector<DirEntry>>;
/// 递归遍历（`max_depth` 为 0 表示仅本层；结果按路径字典序）。
[[nodiscard]] auto walk(std::string_view root, std::uint32_t max_depth = 64) -> Result<std::vector<DirEntry>>;

[[nodiscard]] auto create_directories(std::string_view path) -> Status;
/// 写文件前确保父目录存在。
[[nodiscard]] auto ensure_parent(std::string_view path) -> Status;
[[nodiscard]] auto remove_all(std::string_view path) -> Status;
[[nodiscard]] auto remove_file(std::string_view path) -> Status;
/// 重命名/移动（同盘原子；目标已存在则覆盖）。
///
/// 构建链用它做"先写临时文件、成功再改名"：编译被中断（OOM 杀掉编译器、磁盘写满）时
/// 不会在产物位置留下**半截文件**。半截 `.o` 的阴险之处在它比源文件新——增量判新会认为
/// 它是最新的，于是下一次构建报出一堆莫名其妙的链接错误，而真实原因早就过去了。
[[nodiscard]] auto rename(std::string_view from, std::string_view to) -> Status;
[[nodiscard]] auto copy_file(std::string_view from, std::string_view to) -> Status;
/// 递归复制目录树（目标不存在则创建）。
[[nodiscard]] auto copy_tree(std::string_view from, std::string_view to) -> Status;

[[nodiscard]] auto current_dir() -> Result<std::string>;
[[nodiscard]] auto temp_dir() -> std::string;
[[nodiscard]] auto home_dir() -> std::string;
[[nodiscard]] auto read_env(std::string_view name) -> std::optional<std::string>;

[[nodiscard]] auto join(std::string_view base, std::string_view leaf) -> std::string;
[[nodiscard]] auto normalize(std::string_view path) -> std::string;
[[nodiscard]] auto absolute(std::string_view path) -> Result<std::string>;
[[nodiscard]] auto parent(std::string_view path) -> std::string;
[[nodiscard]] auto file_name(std::string_view path) -> std::string;
[[nodiscard]] auto stem(std::string_view path) -> std::string;
[[nodiscard]] auto extension(std::string_view path) -> std::string;
[[nodiscard]] auto is_absolute(std::string_view path) noexcept -> bool;
/// `from` 相对 `base` 的表示（不解析到父级之外）。
[[nodiscard]] auto relative_to(std::string_view path, std::string_view base) -> std::string;

/// glob 匹配：`*`（段内任意）、`?`（单字符）、`**`（跨目录任意层）；分隔符按 '/' 归一。
[[nodiscard]] auto match_glob(std::string_view pattern, std::string_view path) -> bool;
/// 在 `root` 下展开 glob 模式为实际文件列表（返回相对 root 的路径，字典序）。
///
/// 只遍历模式**字面前缀**对应的子树（`src/core/*.cpp` → 只看 `src/core`）：
/// 全树遍历在“产物目录庞大”的工程里是纯浪费——本仓库实测 14 个模式各自 walk，
/// 3.75s 里 2.9s 花在 `build/` 上（对匹配零贡献），而那是每次构建都付的固定成本。
[[nodiscard]] auto expand_glob(std::string_view root, std::string_view pattern) -> Result<std::vector<std::string>>;
/// 模式里的字面前缀目录（第一个含通配符的段之前）：`src/core/*.cpp` → `src/core`。
[[nodiscard]] auto glob_literal_prefix(std::string_view pattern) -> std::string;

/// 创建唯一临时目录（进程退出不自动清理，测试/工作区用）。
[[nodiscard]] auto make_temp_dir(std::string_view prefix) -> Result<std::string>;

}  // namespace st::fs
