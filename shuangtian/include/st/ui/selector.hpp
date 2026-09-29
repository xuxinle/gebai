#pragma once

/// 选择器（控制通道 `find` 与内部查询共用）：`#id` / `Type` / `Role:state` / `[attr op value]` / `>` 层级。
/// 语法见 `DESIGN.md` §6.3。

#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

class Selector {
 public:
  struct Term {
    enum class Kind : std::uint8_t { Id, Type, Role, Attribute, State, Universal };
    Kind kind{Kind::Universal};
    std::string name{};   ///< 属性名 / 状态名
    std::string op{};     ///< `=` `~=` `^=` `$=` `!=`
    std::string value{};  ///< 属性值 / id / 类型名 / 角色名
  };
  struct Compound {
    std::vector<Term> terms{};
    bool child_combinator{false};  ///< 与前一个 compound 之间是否为 `>`（直接父子）
  };

  static auto parse(std::string_view text) -> Result<Selector>;

  [[nodiscard]] auto matches(const Element& element) const -> bool;
  /// 带层级匹配（`>` 要求直接父子）。
  [[nodiscard]] auto matches_with_ancestors(const Element& element) const -> bool;
  [[nodiscard]] auto text() const -> const std::string& { return text_; }
  [[nodiscard]] auto compounds() const noexcept -> const std::vector<Compound>& { return compounds_; }

 private:
  std::string text_{};
  std::vector<Compound> compounds_{};
};

/// 读取元素上可用于选择器匹配的属性（text/value/role/type/id/checked/…）。
[[nodiscard]] auto element_attribute(const Element& element, std::string_view name) -> std::string;

}  // namespace st::ui
