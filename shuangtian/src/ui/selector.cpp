#include "st/ui/selector.hpp"

#include <algorithm>
#include <format>

#include "st/core/string.hpp"

namespace st::ui {
namespace {

[[nodiscard]] auto match_attribute(std::string_view actual, std::string_view op,
                                   std::string_view expected) -> bool {
  if (op == "=") return actual == expected;
  if (op == "!=") return actual != expected;
  if (op == "~=") return actual.find(expected) != std::string_view::npos;
  if (op == "^=") return actual.starts_with(expected);
  if (op == "$=") return actual.ends_with(expected);
  return false;
}

[[nodiscard]] auto state_matches(const Element& element, std::string_view state) -> bool {
  const SemanticsFlags flags = element.semantics_flags();
  if (state == "visible") return flags.visible;
  if (state == "hidden") return !flags.visible;
  if (state == "focused") return flags.focused;
  if (state == "enabled") return flags.enabled;
  if (state == "disabled") return !flags.enabled;
  if (state == "checked") return flags.checked;
  if (state == "selected") return flags.selected;
  if (state == "hover") return flags.hovered;
  if (state == "pressed") return flags.pressed;
  if (state == "editable") return flags.editable;
  return false;
}

}  // namespace

auto element_attribute(const Element& element, std::string_view name) -> std::string {
  if (name == "id") return element.derived_id();
  if (name == "type") return std::string(element.type());
  if (name == "role") return std::string(to_string(element.role()));
  if (name == "text") return element.semantics_text();
  if (name == "value") return element.semantics_value();
  if (name == "name") {
    const std::string text = element.semantics_text();
    return text.empty() ? element.derived_id() : text;
  }
  const SemanticsFlags flags = element.semantics_flags();
  if (name == "enabled") return flags.enabled ? "true" : "false";
  if (name == "disabled") return flags.enabled ? "false" : "true";
  if (name == "visible") return flags.visible ? "true" : "false";
  if (name == "focused") return flags.focused ? "true" : "false";
  if (name == "checked") return flags.checked ? "true" : "false";
  if (name == "selected") return flags.selected ? "true" : "false";
  return {};
}

auto Selector::parse(std::string_view text) -> Result<Selector> {
  Selector selector;
  selector.text_ = std::string(text);

  std::vector<std::string> compound_texts;
  std::vector<bool> child_combinator;
  std::string current;
  bool pending_child = false;
  bool has_current = false;

  const auto flush = [&]() {
    if (!has_current) return;
    compound_texts.push_back(current);
    child_combinator.push_back(pending_child && compound_texts.size() > 1);
    current.clear();
    has_current = false;
    pending_child = false;
  };

  std::size_t index = 0;
  while (index < text.size()) {
    const char raw = text[index];
    if (raw == ' ' || raw == '\t') {
      if (has_current) flush();
      // 检查接下来的非空字符是否为 '>'（此时不构成后代组合子）
      std::size_t lookahead = index;
      while (lookahead < text.size() && (text[lookahead] == ' ' || text[lookahead] == '\t')) {
        ++lookahead;
      }
      if (lookahead < text.size() && text[lookahead] == '>') {
        index = lookahead;
        continue;
      }
      ++index;
      continue;
    }
    if (raw == '>') {
      if (has_current) flush();
      pending_child = true;
      ++index;
      continue;
    }
    if (raw == '[') {
      const std::size_t close = text.find(']', index);
      if (close == std::string_view::npos) {
        return unexpected(ErrorCode::Parse, std::format("选择器缺少 ']': {}", text));
      }
      current.append(text.substr(index, close - index + 1));
      has_current = true;
      index = close + 1;
      continue;
    }
    current.push_back(raw);
    has_current = true;
    ++index;
  }
  flush();

  if (compound_texts.empty()) {
    return unexpected(ErrorCode::Parse, "选择器为空");
  }

  for (std::size_t compound_index = 0; compound_index < compound_texts.size(); ++compound_index) {
    Compound compound;
    compound.child_combinator = child_combinator[compound_index];
    const std::string& body = compound_texts[compound_index];
    std::size_t cursor = 0;
    while (cursor < body.size()) {
      const char raw = body[cursor];
      if (raw == '#' || raw == '.' || raw == ':') {
        std::size_t end = cursor + 1;
        while (end < body.size() && body[end] != '#' && body[end] != '.' && body[end] != ':' &&
               body[end] != '[') {
          ++end;
        }
        const std::string value = body.substr(cursor + 1, end - cursor - 1);
        Term term;
        term.value = value;
        if (raw == '#') {
          term.kind = Term::Kind::Id;
        } else if (raw == '.') {
          term.kind = Term::Kind::Type;
        } else {
          term.kind = Term::Kind::State;
          term.name = value;
        }
        compound.terms.push_back(std::move(term));
        cursor = end;
        continue;
      }
      if (raw == '[') {
        const std::size_t close = body.find(']', cursor);
        const std::string inner = body.substr(cursor + 1, close - cursor - 1);
        std::size_t op_position = std::string::npos;
        std::string op;
        for (const auto candidate : {"~=", "^=", "$=", "!=", "="}) {
          const std::size_t found = inner.find(candidate);
          if (found != std::string::npos &&
              (op_position == std::string::npos || found < op_position)) {
            op_position = found;
            op = candidate;
          }
        }
        Term term;
        term.kind = Term::Kind::Attribute;
        if (op_position == std::string::npos) {
          term.name = inner;
          term.op = "=";
          term.value = "true";
        } else {
          term.name = inner.substr(0, op_position);
          term.op = op;
          std::string value = inner.substr(op_position + op.size());
          if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) {
            value = value.substr(1, value.size() - 2);
          }
          term.value = value;
        }
        compound.terms.push_back(std::move(term));
        cursor = close + 1;
        continue;
      }
      // 裸类型名
      std::size_t end = cursor;
      while (end < body.size() && body[end] != '#' && body[end] != '.' && body[end] != ':' &&
             body[end] != '[') {
        ++end;
      }
      Term term;
      term.kind = Term::Kind::Type;
      term.value = body.substr(cursor, end - cursor);
      if (!term.value.empty()) compound.terms.push_back(std::move(term));
      cursor = end;
    }
    selector.compounds_.push_back(std::move(compound));
  }
  return selector;
}

auto Selector::matches(const Element& element) const -> bool {
  if (compounds_.empty()) return false;
  const Compound& compound = compounds_.back();
  for (const auto& term : compound.terms) {
    switch (term.kind) {
      case Term::Kind::Universal:
        break;
      case Term::Kind::Id:
        if (element.derived_id() != term.value) return false;
        break;
      case Term::Kind::Type:
        if (element.type() != term.value) return false;
        break;
      case Term::Kind::Role:
        if (to_string(element.role()) != term.value) return false;
        break;
      case Term::Kind::State:
        if (!state_matches(element, term.name)) return false;
        break;
      case Term::Kind::Attribute: {
        const std::string actual = element_attribute(element, term.name);
        if (!match_attribute(actual, term.op, term.value)) return false;
        break;
      }
    }
  }
  return true;
}

auto Selector::matches_with_ancestors(const Element& element) const -> bool {
  if (compounds_.empty()) return false;
  if (!matches(element)) return false;
  if (compounds_.size() == 1) return true;

  const Element* current = &element;
  for (std::size_t index = compounds_.size() - 1; index > 0; --index) {
    const Compound& compound = compounds_[index];
    if (compound.child_combinator) {
      current = current->parent();
      if (current == nullptr) return false;
      // 用祖先自身匹配该 compound
      Selector single;
      single.compounds_.push_back(compounds_[index - 1]);
      if (!single.matches(*current)) return false;
      continue;
    }
    // 后代组合子：向上找任一代匹配
    const Element* probe = current->parent();
    bool found = false;
    while (probe != nullptr) {
      Selector single;
      single.compounds_.push_back(compounds_[index - 1]);
      if (single.matches(*probe)) {
        current = probe;
        found = true;
        break;
      }
      probe = probe->parent();
    }
    if (!found) return false;
  }
  return true;
}

}  // namespace st::ui
