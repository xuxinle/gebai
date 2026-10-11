#pragma once

/// 镂月（Lunaris）图像编辑器 — 文档模型与编辑操作。
///
/// ## 为什么独立成文件
///
/// `main.cpp` 负责声明式界面组装（壳），本文件负责**像素与文档**——两块
/// 各自内聚，测试与 e2e 直接看这里的接口。gbcode 的 `git_service`/`lsp_bridge`
/// 是同一分法。
///
/// ## 撤销为什么是命令式而不是整图快照
///
/// 每笔记录**受影响矩形的前后像素**。整图快照在多层大画布下内存平方级爆炸：
/// 1080p 一层就是 8MB，20 步历史 = 160MB；而一笔通常只动几千像素。
/// 「新建画布」这类一次性整体变化仍可用快照（见 `SnapshotCommand`）。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/surface.hpp"

namespace louyue {

using st::math::Color;
using st::math::Rect;
using st::raster::BlendMode;
using st::raster::Canvas;

/// 混合模式的可选项（与 `raster::BlendMode` 一一对应）。
///
/// 为什么单独给一份而不是直接遍历枚举：**展示顺序**是设计决策
/// （常规 → 加深 → 提亮 → 强光 → 加性），与枚举定义顺序无关。
/// 名字走 `raster::to_string()` 同源——避免界面上写着 "multiply"
/// 而代码里是 `BlendMode::Multiply` 两处各拼一遍。
inline constexpr BlendMode kBlendModes[] = {
    BlendMode::SrcOver, BlendMode::Src,     BlendMode::DstOver, BlendMode::Multiply,
    BlendMode::Screen,  BlendMode::Overlay, BlendMode::Darken,  BlendMode::Lighten,
    BlendMode::Add,
};
inline constexpr std::size_t kBlendModeCount = std::size(kBlendModes);

/// 一个图层：像素缓冲 + 显示属性。
struct Layer {
  std::string name{};
  Canvas pixels;
  bool visible{true};
  /// 不透明度 [0,1]。
  float opacity{1.0f};
  BlendMode blend{BlendMode::SrcOver};

  Layer(std::string layer_name, int width, int height)
      : name(std::move(layer_name)), pixels(width, height, 1.0f) {}
};

/// 撤销栈里的一条命令。
///
/// 接口刻意窄：`undo`/`redo` 各自把状态换回去。具体是「像素补丁」还是
/// 「整层快照」由实现决定——撤销栈不关心。
class Document;   // 前向声明（命令只按引用收文档，不需要完整类型）

class Command {
 public:
  virtual ~Command() = default;
  virtual void undo(Document& document) = 0;
  virtual void redo(Document& document) = 0;
  [[nodiscard]] virtual auto name() const -> std::string = 0;
};

/// 像素补丁：记录某图层上**一个矩形**的前后像素。
///
/// 矩形是调用方算好的「受影响的保守边界」——比逐像素 diff 便宜得多，
/// 而多余的边角像素复原代价可忽略。
class PixelPatchCommand final : public Command {
 public:
  PixelPatchCommand(std::size_t layer, Rect area, std::string label);

  /// 落笔**之前**调用：把该矩形的当前像素存进 `before_`。
  void capture_before(const Document& document);

  /// 落笔**之后**调用：把该矩形的像素存进 `after_`。
  void capture_after(const Document& document);

  /// 补丁是否真的有变化（无变化时不必入栈——空命令会污染历史面板）。
  [[nodiscard]] auto changed() const -> bool;

  /// 尝试把紧随其后的同一条笔画并进自己（**一笔 = 一条历史**）。
  ///
  /// 为什么需要它（量化过的）：高频指针下一次长笔画会产生几百次移动，
  /// 若不合并，历史面板里就是几百条「画笔」——用户按一次撤销只退一小段，
  /// 观感上就是"撤销坏了"。
  ///
  /// 合并条件（三条都要满足，缺一就会把不该并的并起来）：
  /// ① **同一图层**——并了不同图层的补丁会让撤销落到错误的层；
  /// ② **矩形相邻或重叠**——并两个相距很远的补丁等于把中间未改的区域也存进
  ///    前后快照（内存暴涨，且撤销会"顺带复原"中间那些像素）；
  /// ③ 都是"画笔/橡皮"这类同类标签（不同工具的一笔不该并成一条）。
  ///
  /// 合并方式：矩形取并集，`after_` 用新补丁的（它是更新的状态），`before_` **不动**
  ///（撤销要回到这一笔开始之前）——这两条正是"一次移动"与"一整笔"的区别。
  /// ⚠ 需要 `Document` 参与（不是纯粹的两命令相并）：并集矩形里"两块都没覆盖"
  /// 的像素（两矩形接壤但不相交时会出现一条缝）必须以**当前画布值**填进 `before_`。
  /// 它们是"这一笔没碰过的地方"，当前值恰好等于本笔开始前的值——而拿别的值填
  /// 会在撤销时把画布改坏（`write_region` 是逐像素写回，没有"跳过"语义）。
  [[nodiscard]] auto try_merge(const Document& document, const PixelPatchCommand& next) -> bool;

  void undo(Document& document) override;
  void redo(Document& document) override;
  [[nodiscard]] auto name() const -> std::string override { return label_; }

 private:
  void blit(Document& document, const std::vector<std::uint8_t>& source) const;

  std::size_t layer_{0};
  Rect area_{};
  std::string label_{};
  std::vector<std::uint8_t> before_{};
  std::vector<std::uint8_t> after_{};
};

/// 整层快照：用于「新建画布 / 清空 / 缩放」这类**几乎整体改变**的操作。
class LayerSnapshotCommand final : public Command {
 public:
  LayerSnapshotCommand(std::size_t layer, std::string label, Canvas snapshot)
      : layer_(layer), label_(std::move(label)), before_(std::move(snapshot)) {}

  void capture_after(const Document& document);
  void undo(Document& document) override;
  void redo(Document& document) override;
  [[nodiscard]] auto name() const -> std::string override { return label_; }

 private:
  std::size_t layer_{0};
  std::string label_{};
  // `Canvas` 无默认构造（尺寸必得给）——快照在 `capture_after` 里才拿得到尺寸，
  // 所以用 `optional` 延迟构造，而不是给一个 0×0 的假画布。
  Canvas before_;
  std::optional<Canvas> after_{};
};

/// 图像文档：画布尺寸 + 图层（自底向上）+ 历史。
///
/// ⚠ 图层**自底向上**存：`layers[0]` 是最底层。合成时按同样顺序依次叠加，
/// 与绘图软件的图层面板（面板自上而下显示）是**反的**——面板侧负责反转显示，
/// 模型侧保持"合成顺序 = 遍历顺序"，这样 `composite()` 一眼能看懂。
class Document {
 public:
  Document(int width, int height);

  [[nodiscard]] auto width() const noexcept -> int { return width_; }
  [[nodiscard]] auto height() const noexcept -> int { return height_; }

  [[nodiscard]] auto layers() noexcept -> std::vector<Layer>& { return layers_; }
  [[nodiscard]] auto layers() const noexcept -> const std::vector<Layer>& { return layers_; }
  [[nodiscard]] auto active_layer() const noexcept -> std::size_t { return active_; }
  void set_active_layer(std::size_t index);

  /// 当前活动图层的像素（越界时返回空画布的引用——调用方需先查 `has_layer`）。
  [[nodiscard]] auto active_pixels() -> Canvas& { return layers_[active_].pixels; }
  [[nodiscard]] auto has_layer() const noexcept -> bool { return !layers_.empty(); }

  [[nodiscard]] auto dirty() const noexcept -> bool { return dirty_; }
  void set_dirty(bool value) noexcept { dirty_ = value; }
  [[nodiscard]] auto file_path() const -> const std::string& { return file_path_; }
  void set_file_path(std::string path) { file_path_ = std::move(path); }

  // —— 图层操作 ——
  void add_layer(std::string name);
  /// 删除第 `index` 层；最后一层不允许删（文档必须至少有一层）。
  [[nodiscard]] auto remove_layer(std::size_t index) -> bool;
  [[nodiscard]] auto move_layer(std::size_t index, int delta) -> bool;
  /// 把第 `index` 层合并到它**下面**那层（面板上的"向下合并"）。
  [[nodiscard]] auto merge_down(std::size_t index) -> bool;

  /// 把所有可见图层按各自 opacity/blend 合成到 `target`（先清空 target）。
  void composite(Canvas& target) const;

  /// 取合成结果的像素（原图坐标；越界返回透明）。
  [[nodiscard]] auto composite_pixel(int x, int y) const -> Color;

  // —— 历史 ——
  /// 压入一条命令并执行 `redo`（命令应当**已经**被应用到文档上，见下）。
  ///
  /// ⚠ 契约：压栈表示"这一步已经做了"。`push` **不会**再调 `redo`——
  /// 否则刚落的那一笔会被应用两次。重做时才调 `redo`。
  void push(std::unique_ptr<Command> command);

  /// **封笔**：结束当前这一笔，之后推入的补丁不再并进栈顶。
  ///
  /// 为何要显式封：`push` 的合并判据是"同类 + 图层相同 + 位置相邻"——
  /// 而用户"抬起鼠标再在同一处接着画"完全满足这三条。不封的话**两笔会并成一条**，
  /// 撤销一次退回两笔之前（用户会以为撤销退多了）。
  /// 调用点在 `CanvasView::MouseUp`（抬起 = 一笔结束，这是用户的定义）。
  void seal_stroke() noexcept { stroke_sealed_ = true; }

  /// 当前是否处于"可以继续并进栈顶"的状态（测试与诊断用）。
  [[nodiscard]] auto stroke_open() const noexcept -> bool { return !stroke_sealed_; }

  [[nodiscard]] auto undo() -> bool;
  [[nodiscard]] auto redo() -> bool;
  void clear_history();
  [[nodiscard]] auto can_undo() const noexcept -> bool { return cursor_ > 0; }
  [[nodiscard]] auto can_redo() const noexcept -> bool {
    return cursor_ < commands_.size();
  }
  [[nodiscard]] auto history_size() const noexcept -> std::size_t { return commands_.size(); }
  [[nodiscard]] auto history_cursor() const noexcept -> std::size_t { return cursor_; }
  /// 供历史面板显示：`[0, cursor_)` 是已做的步骤名。
  [[nodiscard]] auto history_names() const -> std::vector<std::string>;

  // —— 加载 / 导出 ——
  /// 用 PNG 图像替换整个文档内容（保留尺寸不变则只换像素，尺寸不同则重建图层）。
  [[nodiscard]] auto load_png(const st::codec::PngImage& image) -> bool;
  [[nodiscard]] auto export_png(std::string_view path) const -> bool;

 private:
  int width_{0};
  int height_{0};
  std::vector<Layer> layers_{};
  std::size_t active_{0};
  bool dirty_{false};
  /// 见 `seal_stroke`：`false` = 新补丁可以并进栈顶（同一笔）。
  bool stroke_sealed_{true};
  std::string file_path_{};

  std::vector<std::unique_ptr<Command>> commands_{};
  std::size_t cursor_{0};
};

/// 一笔笔触的**受影响矩形**（含笔刷半径与 1px 外扩）。
///
/// 为何单独暴露：撤销补丁的边界与画笔绘制的边界必须是**同一个算式**——
/// 两处各算一份的话，漏掉的外扩会让“撤销后仍有残边”（亚像素抗锯齿溢出）。
[[nodiscard]] auto stroke_bounds(st::math::Point from, st::math::Point to, float radius)
    -> Rect;

/// 画笔：在两个点之间**插值**描线（高频 MouseMove 下直接画点会断成虚线）。
///
/// 返回受影响矩形的保守边界（含笔刷半径与外扩 1px 的抗锯齿边），
/// 供撤销补丁用。
[[nodiscard]] auto stroke_segment(Canvas& target, st::math::Point from, st::math::Point to,
                                  float radius, Color color, bool erase) -> Rect;

}  // namespace louyue
