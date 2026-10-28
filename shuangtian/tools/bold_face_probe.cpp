/// 探针：本机字体栈里到底有没有真粗体面。
///
/// 为什么需要它：`FontStack::has_bold()` 只决定"粗体走真面还是合成加粗"。
/// 而"禁止合成加粗"是本框架的既定取向——一旦禁用，**没有真粗体面的环境里
/// 所有 bold 文字会静默退化成 Regular**（看起来像"粗体设置失效"）。
/// 所以升级前必须先把这台机器的实情打出来，而不是假设。
///
/// 用法：bold_face_probe

#include <cstdio>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/text/text.hpp"

auto main() -> int {
  const auto loaded = st::text::FontStack::system_default();
  if (!loaded.has_value()) {
    st::log::err("系统字体栈加载失败：{}（无法判断粗体面）\n", loaded.error().message);
    return 1;
  }
  const st::text::FontStack& stack = loaded.value();

  st::log::out("has_bold       = {}\n", stack.has_bold() ? "true" : "false");
  st::log::out("bold face 数量 = {}\n", stack.bold_faces().size());

  st::log::out("\n常规链（{}）：\n", stack.faces().size());
  for (std::size_t i = 0; i < stack.faces().size(); ++i) {
    st::log::out("  [{}] {}\n", i, stack.faces()[i].path());
  }
  st::log::out("\n粗体链（{}）：\n", stack.bold_faces().size());
  for (std::size_t i = 0; i < stack.bold_faces().size(); ++i) {
    st::log::out("  [{}] {}\n", i, stack.bold_faces()[i].path());
  }

  // 中文与拉丁各取一个码点：粗体面必须真的接住，而不是回退常规面。
  // 这一步是"禁用合成加粗"的**前置证据**——没有它，禁用就是盲改。
  const auto probe = [&stack](char32_t codepoint, const char* label) {
    const auto* regular =
        stack.find_face(codepoint, st::text::FontRole::Proportional, false);
    const auto* bold = stack.find_face(codepoint, st::text::FontRole::Proportional, true);
    const bool real = regular != nullptr && bold != nullptr &&
                      regular->path() != bold->path();
    st::log::out("  {:6} regular={}  bold={}  {}\n", label,
              regular != nullptr ? regular->path() : "(none)",
              bold != nullptr ? bold->path() : "(none)",
              real ? "-> 真粗体面生效" : "-> 回退常规面（无真粗体）");
    return real;
  };
  st::log::out("\n码点 -> 字体面：\n");
  const bool cjk_ok = probe(U'粗', "中文");
  const bool latin_ok = probe(U'A', "拉丁");
  const bool digit_ok = probe(U'1', "数字");

  const bool all_ok = cjk_ok && latin_ok && digit_ok;
  st::log::out("\n结论：{}\n",
            all_ok ? "真粗体面覆盖中文/拉丁/数字 —— 可以安全禁用合成加粗"
                   : "存在回退项 —— 禁用合成加粗会让这些字退化成 Regular");
  return all_ok ? 0 : 1;
}
