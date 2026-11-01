/// 桌面应用图标的一行接入：把 **PNG（窗口/任务栏）** 与 **SVG（自绘标题栏）**
/// 两份素材接进 `AppOptions` 与 SVG 注册表。
///
/// ## 为什么要有这个头文件
///
/// 每个桌面应用都要做同样两件事，分散写会漂移：
///
/// 1. `app_options.window_icon_png` ← 嵌入的 PNG 字节
///    （后端 `set_window_icon` 只吃位图，尺寸由系统给 16/32/48）
/// 2. `svg_registry().add_single(id, svg)` ← 品牌标记
///    （`TitleBar` 只在重绘时向注册表取图形，**必须早于首帧**）
///
/// ## 两份素材不是重复
///
/// | | 给谁 | 必须是什么 | 为什么 |
/// |---|---|---|---|
/// | PNG | 窗口系统（`WM_SETICON`） | 位图 | 系统 API 只收位图 |
/// | SVG | 自绘标题栏（框架自己画） | 矢量 | 200% DPI 下要重新光栅化才清晰 |
///
/// 用位图当标题栏图标会在高 DPI 下糊；用矢量当窗口图标系统不认。
/// 所以两份并存，但**同一记号**（同配色同构形）——否则用户看到"两个东西"。
///
/// ## 与 `.ico` 的分工
///
/// `st.pkg` 的 `"icon": "*.ico"` 进的是 **PE 资源**（资源管理器/快捷方式看到的）。
/// 那里的图标和任务栏上那个**不是同一个东西**：任务栏用的是窗口的 `WM_SETICON`，
/// 由下面这份 PNG 决定。只配 `.ico` 不设 `window_icon_png`，任务栏会是空白默认图标。
///
/// ## 失败姿态
///
/// 与 `--theme-file` 加载失败一致：**只记一条日志，不阻止启动**。
/// 图标是外观资源，不是运行前提。
///
/// ## 用法（每个应用四行，路径写死便于 grep）
///
/// ```cpp
/// #include "../app_icon.hpp"
/// // … AppOptions 配好之后、Application 构造之前 …
/// st::app::icon::attach_png(app_options.window_icon_png,
///                           b::embed<"examples/gedou/assets/gedou-icon.png">());
/// st::app::icon::register_svg("gedou", b::embed<"examples/gedou/assets/gedou-logo.svg">());
/// // … 拿到 frame 之后、首帧之前：
/// st::app::icon::apply_brand(frame->title_bar(), "gedou");
/// ```
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "battery/embed.hpp"
#include "st/app/app.hpp"
#include "st/core/log.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/icon.hpp"

namespace st::app::icon {

/// 把嵌入的 PNG 装进 `AppOptions::window_icon_png`。
///
/// 接受 `b::embed<"...">()` 的返回值（有 `.data()` / `.length()`；元素类型是
/// `char`）。用 `std::span` 中转而不是 `reinterpret_cast`：`embed` 的字节本来
/// 就是字节，只是签名叫 `char`——转成 `uint8_t` 是**语义标注**而不是位重解释，
/// 走 span 能把这件事写清楚，也避开禁令（`CONVENTIONS` L6）。
template <typename Asset>
void attach_png(std::vector<std::uint8_t>& target, const Asset& asset) {
  const std::span<const char> bytes(asset.data(), asset.length());
  target.assign(bytes.begin(), bytes.end());
  if (bytes.empty()) st::log::out("图标素材为空（窗口/任务栏图标未设置）\n");
}

/// 注册品牌 SVG，返回是否成功（失败只记日志）。
template <typename Asset>
auto register_svg(std::string_view id, const Asset& asset) -> bool {
  const std::span<const char> bytes(asset.data(), asset.length());
  const std::string_view source(bytes.data(), bytes.size());
  if (!st::ui::svg_registry().add_single(std::string(id), source)) {
    st::log::out("品牌标记注册失败（{}，{} 字节）\n", id, bytes.size());
    return false;
  }
  return true;
}

/// 装载一份 **sprite**（多个 `<symbol id>` 的集合）。
///
/// 与 `register_svg` 的区别：后者是单个 `<svg>` 根，前者是 `<svg>` 下挂一串
/// `<symbol>`（图标库的常见形态）。两者共用同一张注册表，`svg:` 名字都能查。
///
/// ⚠ 顺序无所谓（`IconSet::load` 是**追加**语义、同名覆盖），但**必须在首帧前**。
template <typename Asset>
auto load_sprite(const Asset& asset) -> bool {
  const std::span<const char> bytes(asset.data(), asset.length());
  const std::string_view source(bytes.data(), bytes.size());
  if (!st::ui::svg_registry().load_sprite(source)) {
    st::log::out("SVG sprite 装载失败（{} 字节）\n", bytes.size());
    return false;
  }
  return true;
}

/// 把品牌标记设到标题栏（`svg:` 前缀 = 强制走 SVG 源）。
///
/// ⚠ 必须在**首帧渲染之前**调用：注册表是显式装载的（无静态初始化器），
/// `TitleBar` 只在重绘时取图形；晚一步不报错，只表现为图标不出现。
///
/// 另外：**不要在它之后再调 `set_icon("某个内置名")`**——那会把品牌标记覆盖掉。
/// 实测踩过：批量接入时旧代码末尾还留着一行 `set_icon("brush")`，标题栏显示的是笔形。
inline void apply_brand(st::ui::TitleBar* title_bar, std::string_view id) {
  if (title_bar == nullptr) return;
  // 显式拼接（不用 `"svg:" + std::string(id)`）：这里把意图写全——**前缀必须存在**，
  // 否则会被当成内置图标名去查、查不到就静默不画。
  std::string name = "svg:";
  name.append(id.data(), id.size());
  title_bar->set_icon(name);
}

}  // namespace st::app::icon
