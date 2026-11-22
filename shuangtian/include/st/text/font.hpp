#pragma once

/// 字体解析（text 层）：TrueType（`glyf`/`loca`/`cmap`/`hmtx`/`kern`）、OpenType CFF（`CFF ` +
/// Type2 charstring）、集合字体 TTC/OTC（`ttcf` 多 face）、CID-keyed CFF（`FDArray`/`FDSelect`/
/// `Private`/`Subrs`）。全自研、零第三方依赖。
///
/// 约定：
/// - **安全失败**：任何长度不足/偏移越界/结构损坏的字体一律返回 `Error`（`Parse`/`Invalid`），
///   绝不越界读；`FontFace::load` 失败时对象不可用。
/// - **坐标**：`glyph_outline` 返回**字体单位**、**y 向上**、原点在**基线笔位**（pen origin）的路径；
///   缩放（`size / units_per_em`）与 y 轴翻转（画布 y 向下）由 `TextRenderer` 负责。
/// - **缓存**：字形轮廓按 face 缓存（`GlyphId → Path`），重复取同一字形为常数时间。
/// - **拷贝语义**：`FontFace` 可拷贝，副本共享不可变的字体数据与轮廓缓存（内部 `shared_ptr`）。

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "st/core/error.hpp"
#include "st/math/color.hpp"
#include "st/math/path.hpp"

namespace st::text {

/// 字形索引（字体内部编号；0 恒为 `.notdef`）。
using GlyphId = std::uint32_t;

/// 字体级度量（除 `glyph_count` 外均为**字体单位**）。
struct FontMetrics {
  float units_per_em{1000.0f};
  float ascender{0.0f};
  float descender{0.0f};  ///< 负数
  float line_gap{0.0f};
  std::uint32_t glyph_count{0};
};

/// 字体**自带**的笔画提示（CFF `hstem`/`vstem`/`hstemhm`/`vstemhm`，字体单位）。
///
/// 为什么需要它：CFF（OTF）字体把"笔画在哪、多宽"直接写在 charstring 的 stem hints 里，
/// 而按轮廓几何反推笔画（两两配对直线边）的**召回只有 ~19%**（实测，见
/// `docs/BACKLOG.md` 的拟合漏斗）——因为汉字大量笔画带微斜度或由曲线构成，配不上对，
/// 于是同一字里一部分笔画被吸附、另一部分留着分数相位，正是"线条粗细不均匀"的来源。
/// 有了自带 hints，笔画位置与宽度都是**字体设计者给的**，召回不再靠猜。
///
/// `lo`/`hi` 是横轴（vstem 为 x、hstem 为 y）上的两个边缘；**顺序不保证**（取 `min`/`max`）。
/// 无 hint 的字体（TrueType `glyf` 轮廓）返回空——那时仍走几何拟合那条路。
struct StemHint {
  float lo{0.0f};
  float hi{0.0f};
  bool vertical{true};  ///< true = `vstem`（竖笔画，横轴是 x）
};

/// 单字形度量（字体单位）。
struct Glyph {
  GlyphId id{0};
  float advance{0.0f};
  float bearing_x{0.0f};
  float bearing_y{0.0f};
  /// 字形包围盒的**底边**（字体单位，基线上为正）。
  ///
  /// 与 `bearing_y` 配对才拿得到墨迹的垂直跨度——而那是"把一行字在一片高度里
  /// 真正居中"的唯一依据（行盒/ascent 都含字体预留的头尾空间，见
  /// `TextRenderer::ink_metrics`）。
  /// 字母通常为 0（坐在基线上），`g`/`，` 这类下探字形为负。
  float ink_bottom{0.0f};
  bool empty{true};
};

/// 彩色字形的一层（`COLR` v0 的 `LayerRecord` + `CPAL` 取色）。
///
/// 彩色 emoji 在字体里的表达是**分层绘制**：一个基础字形不是一段轮廓，而是
/// 「若干层，每层是另一个字形 + 一个颜色」。按层顺序逐层填充即得彩色字形——
/// 这不需要位图、不需要着色器，与普通字形走同一条覆盖率渲染路径。
struct ColorLayer {
  GlyphId glyph{0};
  /// 该层的颜色（`CPAL` 第 0 号调色板；alpha 已按字体给的 alpha 复合到 `a`）。
  /// `use_text_color` 为真时本字段无意义。
  math::Color color{0, 0, 0, 255};
  /// `paletteIndex == 0xFFFF`：该层用**文字色**而不是调色板色（`COLR` 规范的
  /// "text foreground color" 约定）。等宽图标字体里的单色层就是这种。
  bool use_text_color{false};
};

/// 单个字体文件（或集合字体的单个 face）。
class FontFace {
 public:
  FontFace() = default;

  /// 加载字体文件/face。
  /// @param path 字体文件路径（TTF/OTF/TTC/OTC）。
  /// @param face_index 集合字体的 face 序号（0 起；非集合字体只接受 0）。
  /// @return 失败：`Io` 读文件失败、`Parse` 非字体/结构损坏/截断、`NotFound` face 序号越界。
  [[nodiscard]] static auto load(std::string_view path, int face_index = 0) -> Result<FontFace>;

  /// 码点 → 字形索引（经 `cmap` 格式 4/6/12/0）。
  /// @return 失败：`NotFound` 字体不含该码点。
  [[nodiscard]] auto glyph_index(char32_t codepoint) const -> Result<GlyphId>;

  /// 字形度量（`advance`/`bearing` 为字体单位；`empty` 表示无轮廓，如空格）。
  /// @return 失败：`NotFound` 字形索引越界。
  [[nodiscard]] auto glyph(GlyphId id) const -> Result<Glyph>;

  /// 码点 → 字形度量（`glyph_index` + `glyph` 的便捷组合）。
  [[nodiscard]] auto glyph_for(char32_t codepoint) const -> Result<Glyph>;

  /// 是否覆盖该码点。
  [[nodiscard]] auto has_glyph(char32_t codepoint) const -> bool;

  /// 字体度量（默认构造的 face 返回零值）。
  [[nodiscard]] auto metrics() const noexcept -> const FontMetrics&;

  /// 字体名（`name` 表的 full name / family name；缺表时为空串）。
  [[nodiscard]] auto name() const -> const std::string&;

  /// 加载时使用的字体文件路径。
  [[nodiscard]] auto path() const -> const std::string&;

  /// 是否是 **CFF（OTF）轮廓**字体（`glyf` 缺席且有 `CFF ` 表）。
  ///
  /// 为什么需要它：**笔画加墨（stem darkening）只该对 CFF 字体做**——
  /// 这是 FreeType 自身的规则（`cff` 引擎的 `no-stem-darkening` 默认为 FALSE；
  /// 而 TrueType 的指令本身就保证笔画不为零宽，再加墨会过粗）。
  /// 实测（2026-10-06）：同字号下 DejaVu（真型）已经比浏览器**深 1.09**，
  /// 而 Noto Sans CJK（CFF）只有 0.89——不对字体类型分档就会"补了 CFF、过深了 TTF"。
  [[nodiscard]] auto is_cff() const noexcept -> bool;

  /// 加载时使用的 face 序号。
  [[nodiscard]] auto face_index() const noexcept -> int;

  /// 字形轮廓（**字体单位、y 向上、原点在基线笔位**）。
  /// @return 失败：`NotFound` 索引越界、`Parse` 轮廓数据损坏、`Unsupported` 不支持的轮廓字符
  ///         串操作（如 Type2 `seac` 组合字形）。
  [[nodiscard]] auto glyph_outline(GlyphId id) const -> Result<math::Path>;

  /// `kern` 表（格式 0）字距调整，**字体单位**；无该对返回 0（`noexcept`，不报错）。
  [[nodiscard]] auto kerning(GlyphId left, GlyphId right) const noexcept -> float;

  /// `ccmp` 的单字替换（字形 → 字形）；未命中返回 `nullopt`。
  ///
  /// 用途：`uni2764`（文本表现的心形）在 emoji 字体里先被 `ccmp` 换成彩色基础字形，
  /// 不做这一步就只能画出一个单色轮廓。
  [[nodiscard]] auto compose_single(GlyphId id) const -> std::optional<GlyphId>;

  /// `ccmp` 的多字合字：给定**候选序列**（已逐个字形号），返回合成后的字形与
  /// 被吃掉的字形数。未命中返回 `nullopt`。
  ///
  /// `glyphs` 至少要看 1 个元素（首字形）；长度不够匹配最长规则时按已有前缀试。
  /// 返回的 pair 为 `{合成字形, 参与合字的字形数}`。
  [[nodiscard]] auto compose_ligature(std::span<const GlyphId> glyphs) const
      -> std::optional<std::pair<GlyphId, std::size_t>>;

  /// 该字形**能不能开头合字**（整形循环的快路径判据；不能就不必展开候选）。
  [[nodiscard]] auto has_ligature(GlyphId id) const noexcept -> bool;

  /// 该字形的**彩色分层**（`COLR` v0 基础字形记录 + `CPAL` 第 0 号调色板）。  ///
  /// 返回**层序**（先画的在前）。空表有两种含义，调用方都按"按单色轮廓画"处理：
  /// ① 该字形不是彩色字形（普通文字、以及 `COLR` v1 画笔图但与 v0 记录不重叠的字形）；
  /// ② 字体没有 `COLR`/`CPAL` 表。
  ///
  /// 结果按字形缓存（与轮廓缓存同一生命周期），返回的是缓存内的引用。
  [[nodiscard]] auto color_layers(GlyphId id) const -> const std::vector<ColorLayer>&;

  /// 字体是否带彩色分层表（`COLR` 有 v0 基础字形记录且 `CPAL` 有颜色记录）。
  ///
  /// 与 `color_layers(id).empty()` 的分工：本函数回答"这个字体有没有彩色能力"，
  /// 它决定调用方要不要为彩色路径预先付代价（缓存、分支）；单字形层面仍以
  /// `color_layers` 是否为空为准。
  [[nodiscard]] auto has_color_layers() const noexcept -> bool;

  /// 该字形**字体自带**的笔画提示（字体单位；顺序即 charstring 里的出现顺序）。
  ///
  /// 仅 CFF（OTF）字体有；`glyf` 轮廓或解释失败时返回空表（调用方回退到几何拟合）。
  /// 结果按字形缓存（与轮廓缓存同一生命周期）。
  [[nodiscard]] auto stem_hints(GlyphId id) const -> const std::vector<StemHint>&;

 private:
  struct Data;  ///< 内部实现（见 `src/text/font.cpp`；不可变解析结果 + 缓存）
  std::shared_ptr<Data> data_{};
};

/// 该字体文件包含的 face 数量（非集合字体为 1）。
///
/// （彩色分层见 `FontFace::color_layers`；`COLR` v1 的画笔图尚未解释，见 `docs/BACKLOG.md`。）
///
/// **存在的理由：不要把「猜下标」当探测**（2026-10-08 量化）：`FontStack` 要在集合字体
/// （`.ttc`/`.otc`）里挑简体 face，原本的做法是 `index = 0..11` 逐个 `load` 到失败为止。
/// 而 `load` **先把整个文件读进内存**（`msyh.ttc` 20 MB），越过界那一次一样要读完才报
/// `NotFound`——实测单次 9–12 ms，且「失败不进缓存」使它**每次调用都重付**。
/// 先问 face 数就不会产生越界尝试。
[[nodiscard]] auto font_face_count(std::string_view path) -> Result<std::uint32_t>;

/// 字体侧缓存计数器（**仅用于测试与诊断**）。
///
/// 为什么要它（2026-10-08）：护「字体栈解析被缓存」只能用**耗时阈值**
/// （`tests/text_font_default_test.cpp` 曾写 `hot_ms < 1.0`）——那在共享机器上两个方向
/// 都会翻车，且不能区分「缓存命中」与「机器恰好很快」。耗时阈值也确实误了事：
/// 同一个缺陷（`load_face` 猜 face 下标 → 越界尝试每次重读 20 MB 字体，实测 44 ms）
/// 被当成“环境抖动”放过了。改成数**确定量**：缓存命中/落空的次数。
struct FontCacheStats {
  /// `FontFace::load`（含读盘）真正跑过的次数。
  std::size_t face_loads{0};
  /// 命中已解析 face 而直接返回的次数。
  std::size_t face_hits{0};
  /// `font_face_count`（读文件头）真正跑过的次数。
  std::size_t count_reads{0};
  /// 加载时**走到内存映射**的次数（2026-10-14 新增）。
  ///
  /// 为何要这个数："字体不再整读"是个**机制断言**，而它无法从内存数字上稳定验证
  /// （共享机器上 RSS 受 GC/换页影响）。数"这次加载走的哪条路"则是确定量。
  std::size_t mapped_loads{0};
  /// 加载时映射不可用、**退回整读进 vector** 的次数。
  ///
  /// 正常情况下恒为 0；非 0 说明挂了不支持映射的文件系统（网络盘等）——
  /// 那时内存指标回到改造前，但功能不受影响（这正是保留回退的理由）。
  std::size_t read_fallbacks{0};
};

/// 取当前计数。
///
/// **进程级**（2026-10-14 改，此前是 `thread_local`）：缓存本体已改为进程级共享
/// （`load_face_cached`），计数器要是还线程局部就**测不出"其他线程有没有重复读盘"**
/// ——而那正是本轮要防的隐患（Tasks 池 8 线程最坏 8×86MB）。
[[nodiscard]] auto font_cache_stats() noexcept -> FontCacheStats;

/// 计数自增入口（进程级原子；只有 `font.cpp` / `text.cpp` 的内部路径调）。
///
/// 为何不返回可变的 `FontCacheStats&`：多线程同时自增也需要原子性，
/// 而返回引用必然诱使调用方写 `stats.face_loads += 1`（非原子读改写）。
/// **不是公开语义的一部分**：外部只读 `font_cache_stats()`。
void note_face_load() noexcept;
void note_face_hit() noexcept;
void note_face_count_read() noexcept;
void note_face_mapped() noexcept;
void note_face_read_fallback() noexcept;

}  // namespace st::text
