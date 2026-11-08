#pragma once

/// @file key_value_row.hpp
/// @brief `KeyValueRow`：键值行（左标签右值，用于详情面板）。。
///
/// 从 `basic.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `basic.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <string>
#include <string_view>
#include "st/ui/component_id.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 键值行（左标签右值，用于详情面板）。
class KeyValueRow : public Element {
 public:
  KeyValueRow(std::string key, std::string value);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "KeyValueRow"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Text; }

  void set_value(std::string value);
  [[nodiscard]] auto value() const -> const std::string& { return value_; }
  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return label_; }
  [[nodiscard]] auto semantics_value() const -> std::string override { return value_; }

 private:
  /// 显示标签（**不是** `Element::key_`）：基类的 key 是稳定逻辑身份（参与自动 id
  /// `Type@key`），把界面文案当身份会让 id 随文案变动而漂移。
  std::string label_{};
  std::string value_{};
};

}  // namespace st::ui

ST_UI_COMPONENT_WITH(::st::ui::KeyValueRow, "KeyValueRow",
                     std::make_unique<::st::ui::KeyValueRow>("", ""))
