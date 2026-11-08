#pragma once

/// @file heading.hpp
/// @brief `Heading`：标题（按层级取字号与字重）。。
///
/// 从 `basic.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `basic.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include "st/ui/component_id.hpp"
#include "st/ui/components/text.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 标题（按层级取字号与字重）。
class Heading : public Text {
 public:
  explicit Heading(std::string content = {}, std::uint32_t level = 1);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Heading"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Heading; }
  void set_level(std::uint32_t level);
  [[nodiscard]] auto level() const noexcept -> std::uint32_t { return level_; }
  void apply_theme(const Theme& theme) override;

 private:
  std::uint32_t level_{1};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Heading, "Heading")
