#pragma once

/// @file heading.hpp
/// @brief `Heading`：标题（按层级取字号与字重）。。
///
/// 从 `basic.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `basic.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/component_registry.hpp"
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

  // 属性面：`level` 必须能从 XML / 控制通道 / 脚本写。
  //
  // 为何要三个方法都覆写：三条入口共用同一份属性面（`build_from_xml`、
  // 协议 `set`、脚本桥都走 `property_names` + `set_property`）。只实现 `set_level`
  // （C++ 入口）等于“这个能力对外不可见”——XML 写 `level` 会被报「值没被接受」。
  [[nodiscard]] auto get_property(std::string_view name) const
      -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  std::uint32_t level_{1};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Heading, "Heading")
