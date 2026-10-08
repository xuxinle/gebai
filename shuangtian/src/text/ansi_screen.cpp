/// ANSI 屏幕模型实现（见 `ansi_screen.hpp` 的设计说明）。
///
/// 结构：**解析器 + 网格**两段。解析器是一个状态机（`State`），把字节流切成
/// "文本"与"指令"；网格是 `rows × cols` 的格子，指令按终端语义改它。
///
/// 两个容易写错的地方（都会表现为"大部分时候对、偶尔乱"）：
///
/// 1. **跨调用的残片**：PTY 一次读到的字节与序列边界无关——半个转义序列、
///    半个 UTF-8 码点都可能出现在分片边界上。所以 `utf8_pending_` 与 `state_`
///    都是**跨 `feed` 保留**的成员，不能当局部变量。
/// 2. **宽字符占两列**：CJK/emoji 占两格，右半格是 `continuation` 标记。
///    光标要一次走两列，且不能把右半格当独立字符画（否则整行错位）。

#include "st/text/ansi_screen.hpp"

#include <algorithm>
#include <format>

namespace st::text {

namespace {

/// 默认前景/背景色（`SGR 39`/`49` 回到它）。
constexpr AnsiColor kDefaultColor{};

/// 把 UTF-8 首字节的**期望长度**算出来（0 = 非法字节，当单字节处理）。
[[nodiscard]] auto utf8_length(unsigned char lead) -> int {
  if (lead < 0x80U) return 1;
  if ((lead & 0xE0U) == 0xC0U) return 2;
  if ((lead & 0xF0U) == 0xE0U) return 3;
  if ((lead & 0xF8U) == 0xF0U) return 4;
  return 1;
}

/// 从 UTF-8 字节算出码点（调用方保证长度足够且合法）。
[[nodiscard]] auto utf8_decode(const std::string_view bytes) -> char32_t {
  const auto byte = [&](std::size_t index) -> char32_t {
    return index < bytes.size() ? static_cast<char32_t>(static_cast<unsigned char>(bytes[index]))
                                : 0U;
  };
  const char32_t lead = byte(0);
  if (lead < 0x80U) return lead;
  if ((lead & 0xE0U) == 0xC0U) return ((lead & 0x1FU) << 6U) | (byte(1) & 0x3FU);
  if ((lead & 0xF0U) == 0xE0U) {
    return ((lead & 0x0FU) << 12U) | ((byte(1) & 0x3FU) << 6U) | (byte(2) & 0x3FU);
  }
  return ((lead & 0x07U) << 18U) | ((byte(1) & 0x3FU) << 12U) | ((byte(2) & 0x3FU) << 6U) |
         (byte(3) & 0x3FU);
}

/// 码点显示宽度：0（组合符/控制）、1（半角）、2（全角）。
///
/// 只覆盖终端里实际会出现的范围（CJK、全角标点、常见 emoji），不追求
/// Unicode 全表——查表代价要摊在**每个字符**上，而 99% 的输出是 ASCII。
[[nodiscard]] auto cell_width(char32_t ch) -> int {
  if (ch == 0) return 0;
  if (ch < 0x20U) return 0;                  // 控制字符无宽度
  if (ch >= 0x7FU && ch < 0xA0U) return 0;   // DEL 与 C1
  if (ch == 0x200BU) return 0;               // 零宽空格
  if (ch >= 0x0300U && ch <= 0x036FU) return 0;   // 组合附加符号
  // 东亚宽字符的主要区段（与 wcwidth 的判据同源，取交集避免误伤拉丁）。
  const bool wide =
      (ch >= 0x1100U && ch <= 0x115FU) ||     // 韩文字母
      (ch >= 0x2E80U && ch <= 0x303EU) ||     // CJK 部首、标点
      (ch >= 0x3041U && ch <= 0x33FFU) ||     // 假名、CJK 兼容
      (ch >= 0x3400U && ch <= 0x4DBFU) ||     // CJK 扩展 A
      (ch >= 0x4E00U && ch <= 0x9FFFU) ||     // CJK 统一表意
      (ch >= 0xA000U && ch <= 0xA4CFU) ||     // 彝文
      (ch >= 0xAC00U && ch <= 0xD7A3U) ||     // 韩文音节
      (ch >= 0xF900U && ch <= 0xFAFFU) ||     // CJK 兼容表意
      (ch >= 0xFE30U && ch <= 0xFE6FU) ||     // CJK 兼容形式
      (ch >= 0xFF00U && ch <= 0xFF60U) ||     // 全角形式
      (ch >= 0xFFE0U && ch <= 0xFFE6U) ||
      (ch >= 0x1F300U && ch <= 0x1F9FFU) ||   // emoji
      (ch >= 0x20000U && ch <= 0x3FFFDUL);    // CJK 扩展 B 及以后
  return wide ? 2 : 1;
}

/// 把 UTF-8 码点编回字节（`row_text` 用）。
auto utf8_encode(char32_t ch, std::string& out) -> void {
  if (ch < 0x80U) {
    out.push_back(static_cast<char>(ch));
  } else if (ch < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (ch >> 6U)));
    out.push_back(static_cast<char>(0x80U | (ch & 0x3FU)));
  } else if (ch < 0x10000U) {
    out.push_back(static_cast<char>(0xE0U | (ch >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((ch >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (ch & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (ch >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((ch >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((ch >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (ch & 0x3FU)));
  }
}

}  // namespace

AnsiScreen::AnsiScreen(int cols, int rows) {
  cols_ = std::max(1, cols);
  rows_ = std::max(1, rows);
  screen_.assign(static_cast<std::size_t>(rows_), std::vector<AnsiCell>(
                                                      static_cast<std::size_t>(cols_)));
  alt_screen_ = screen_;
  dirty_.assign(static_cast<std::size_t>(rows_), true);
  scroll_bottom_ = rows_ - 1;
}

auto AnsiScreen::line(int row) -> std::vector<AnsiCell>& {
  return screen_[static_cast<std::size_t>(std::clamp(row, 0, rows_ - 1))];
}

auto AnsiScreen::line(int row) const -> const std::vector<AnsiCell>& {
  return screen_[static_cast<std::size_t>(std::clamp(row, 0, rows_ - 1))];
}

auto AnsiScreen::cell(int row, int col) const -> const AnsiCell& {
  static const AnsiCell kBlank{};
  if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return kBlank;
  return line(row)[static_cast<std::size_t>(col)];
}

void AnsiScreen::set_dirty(int row) {
  if (row >= 0 && row < rows_) dirty_[static_cast<std::size_t>(row)] = true;
}

void AnsiScreen::mark_all_dirty() {
  std::fill(dirty_.begin(), dirty_.end(), true);
}

auto AnsiScreen::take_dirty_rows() -> std::vector<int> {
  std::vector<int> out;
  for (int row = 0; row < rows_; ++row) {
    if (dirty_[static_cast<std::size_t>(row)]) {
      out.push_back(row);
      dirty_[static_cast<std::size_t>(row)] = false;
    }
  }
  return out;
}

// ════════════════════════════════════════════════════════════════════════════
// 内容读出
// ════════════════════════════════════════════════════════════════════════════

auto AnsiScreen::row_text(int row) const -> std::string {
  if (row < 0 || row >= rows_) return {};
  const auto& cells = line(row);
  // 先找最后一个非空格子：行尾空白不留（`ls -l` 之类的右对齐才不会带一堆空格）。
  std::size_t last = cells.size();
  while (last > 0) {
    const AnsiCell& c = cells[last - 1];
    if (c.ch != U' ' && !c.continuation) break;
    --last;
  }
  std::string out;
  for (std::size_t index = 0; index < last; ++index) {
    const AnsiCell& c = cells[index];
    if (c.continuation) continue;   // 宽字符的右半格不单独输出
    utf8_encode(c.ch, out);
  }
  return out;
}

auto AnsiScreen::plain_text() const -> std::string {
  std::string out;
  for (int row = 0; row < rows_; ++row) {
    out += row_text(row);
    if (row + 1 < rows_) out.push_back('\n');
  }
  return out;
}

auto AnsiScreen::scrollback_line(std::size_t index) const -> std::string {
  if (index >= scrollback_.size()) return {};
  const auto& cells = scrollback_[index];
  std::size_t last = cells.size();
  while (last > 0) {
    const AnsiCell& c = cells[last - 1];
    if (c.ch != U' ' && !c.continuation) break;
    --last;
  }
  std::string out;
  for (std::size_t i = 0; i < last; ++i) {
    if (cells[i].continuation) continue;
    utf8_encode(cells[i].ch, out);
  }
  return out;
}

// ════════════════════════════════════════════════════════════════════════════
// 屏幕操作
// ════════════════════════════════════════════════════════════════════════════

void AnsiScreen::clear_row(int row, int from_col) {
  if (row < 0 || row >= rows_) return;
  auto& cells = line(row);
  for (int col = std::max(0, from_col); col < cols_; ++col) {
    cells[static_cast<std::size_t>(col)] = AnsiCell{};
  }
  set_dirty(row);
}

void AnsiScreen::push_scrollback(const std::vector<AnsiCell>& row) {
  // 备用屏**不进**回看：全屏程序的重绘不是"历史"（vim 滚出的行没有意义），
  // 而且它会污染用户 `↑` 能翻到的主屏内容。
  if (alt_active_) return;
  scrollback_.push_back(row);
  while (scrollback_.size() > kMaxScrollback) scrollback_.pop_front();
}

void AnsiScreen::scroll_up(int top, int bottom, int count) {
  top = std::max(0, top);
  bottom = std::min(rows_ - 1, bottom);
  if (top > bottom) return;
  for (int i = 0; i < count; ++i) {
    push_scrollback(line(top));
    for (int row = top; row < bottom; ++row) {
      line(row) = line(row + 1);
    }
    clear_row(bottom, 0);
  }
  for (int row = top; row <= bottom; ++row) set_dirty(row);
}

void AnsiScreen::scroll_down(int top, int bottom, int count) {
  top = std::max(0, top);
  bottom = std::min(rows_ - 1, bottom);
  if (top > bottom) return;
  for (int i = 0; i < count; ++i) {
    for (int row = bottom; row > top; --row) {
      line(row) = line(row - 1);
    }
    clear_row(top, 0);
  }
  for (int row = top; row <= bottom; ++row) set_dirty(row);
}

void AnsiScreen::resize(int cols, int rows) {
  cols = std::max(1, cols);
  rows = std::max(1, rows);
  if (cols == cols_ && rows == rows_) return;

  // 重排策略：**保留左上角的内容**（与多数终端的做法一致）。缩窄时右侧被截，
  // 但内容本身不丢（用户 `↑` 还能在主屏里找到），扩宽时补空格。
  auto remap = [&](std::vector<std::vector<AnsiCell>>& target) {
    std::vector<std::vector<AnsiCell>> next(static_cast<std::size_t>(rows),
                                            std::vector<AnsiCell>(static_cast<std::size_t>(cols)));
    for (int row = 0; row < std::min(rows, static_cast<int>(target.size())); ++row) {
      const auto& source = target[static_cast<std::size_t>(row)];
      auto& destination = next[static_cast<std::size_t>(row)];
      const std::size_t copy = std::min(source.size(), static_cast<std::size_t>(cols));
      std::copy_n(source.begin(), copy, destination.begin());
      // 切窄正好切开宽字符：界内留了左半、右半被截——左半补空（半字无法显示）。
      // 判据：**被截掉的首格**（`source[copy]`）是 continuation，且界内末格是宽字主格。
      if (copy > 0 && copy < source.size() && source[copy].continuation &&
          !destination[copy - 1].continuation && cell_width(destination[copy - 1].ch) == 2) {
        destination[copy - 1] = AnsiCell{};
      }
      // 防御：孤儿 continuation（主格不在界内）同样补空。
      if (copy > 0 && destination[copy - 1].continuation) destination[copy - 1] = AnsiCell{};
    }
    target = std::move(next);
  };
  remap(screen_);
  remap(alt_screen_);

  cols_ = cols;
  rows_ = rows;
  dirty_.assign(static_cast<std::size_t>(rows), true);
  cursor_row_ = std::clamp(cursor_row_, 0, rows_ - 1);
  cursor_col_ = std::clamp(cursor_col_, 0, cols_ - 1);
  scroll_top_ = 0;
  scroll_bottom_ = rows_ - 1;
  wrap_pending_ = false;
}

// ════════════════════════════════════════════════════════════════════════════
// 写字符
// ════════════════════════════════════════════════════════════════════════════

void AnsiScreen::put(char32_t ch) {
  const int width = cell_width(ch);
  if (width == 0) return;   // 组合符等：本层不做叠加，直接忽略（不污染屏幕）

  // 挂起态：上一次写满了一行，这次才真换行（`DECAWM` 的语义）。
  if (wrap_pending_ && auto_wrap_) {
    cursor_col_ = 0;
    newline();
  }
  wrap_pending_ = false;

  // 行尾放不下宽字符时，按终端惯例先换行（否则会把两格劈开）。
  if (width == 2 && cursor_col_ + 1 >= cols_) {
    if (auto_wrap_) {
      cursor_col_ = 0;
      newline();
    } else {
      return;
    }
  }

  auto& cells = line(cursor_row_);
  cells[static_cast<std::size_t>(cursor_col_)] = AnsiCell{.ch = ch, .style = style_};
  last_graphic_ = ch;   // REP（CSI b）重复的是「上个真正写入的字符」
  if (width == 2 && cursor_col_ + 1 < cols_) {
    cells[static_cast<std::size_t>(cursor_col_ + 1)] =
        AnsiCell{.ch = U' ', .style = style_, .continuation = true};
  }
  set_dirty(cursor_row_);

  cursor_col_ += width;
  if (cursor_col_ >= cols_) {
    cursor_col_ = cols_ - 1;
    wrap_pending_ = true;   // 停在最后一格，等下一个字符
  }
}

void AnsiScreen::newline() {
  // 在滚动区底部换行 = 滚屏（这是"输出不断往下走"的实现）。
  if (cursor_row_ == scroll_bottom_) {
    scroll_up(scroll_top_, scroll_bottom_, 1);
  } else if (cursor_row_ < rows_ - 1) {
    ++cursor_row_;
  }
  wrap_pending_ = false;
}

void AnsiScreen::carriage_return() {
  cursor_col_ = 0;
  wrap_pending_ = false;
}

void AnsiScreen::tab() {
  // 制表位按 8 列（与终端惯例一致）。
  const int next = ((cursor_col_ / 8) + 1) * 8;
  cursor_col_ = std::min(next, cols_ - 1);
  wrap_pending_ = false;
}

void AnsiScreen::backspace() {
  if (cursor_col_ > 0) --cursor_col_;
  wrap_pending_ = false;
}

// ════════════════════════════════════════════════════════════════════════════
// 解析
// ════════════════════════════════════════════════════════════════════════════

auto AnsiScreen::param(std::size_t index, int fallback) const -> int {
  if (index >= params_.size()) return fallback;   // 参数不够 → 终端规范里就是取默认值
  const int value = params_[index];
  return value < 0 ? fallback : value;            // 显式空参数（`;;`）同样取默认值
}

void AnsiScreen::reset_style() { style_ = AnsiStyle{}; }

void AnsiScreen::apply_sgr() {
  if (params_.empty()) {
    reset_style();   // `CSI m` 无参数 = 复位（这是规范，不是特例）
    return;
  }
  for (std::size_t index = 0; index < params_.size(); ++index) {
    const int code = params_[index] < 0 ? 0 : params_[index];
    switch (code) {
      case 0: reset_style(); break;
      case 1: style_.bold = true; break;
      case 2: style_.dim = true; break;
      case 3: style_.italic = true; break;
      case 4: style_.underline = true; break;
      case 5: style_.blink = true; break;
      case 7: style_.reverse = true; break;
      case 9: style_.strike = true; break;
      case 22: style_.bold = false; style_.dim = false; break;
      case 23: style_.italic = false; break;
      case 24: style_.underline = false; break;
      case 25: style_.blink = false; break;
      case 27: style_.reverse = false; break;
      case 29: style_.strike = false; break;
      case 39: style_.fg = kDefaultColor; break;
      case 49: style_.bg = kDefaultColor; break;
      default: break;
    }
    // 前景 30-37 / 90-97，背景 40-47 / 100-107。
    if (code >= 30 && code <= 37) style_.fg = AnsiColor::indexed(static_cast<std::uint8_t>(code - 30));
    else if (code >= 90 && code <= 97) style_.fg = AnsiColor::indexed(static_cast<std::uint8_t>(code - 90 + 8));
    else if (code >= 40 && code <= 47) style_.bg = AnsiColor::indexed(static_cast<std::uint8_t>(code - 40));
    else if (code >= 100 && code <= 107) style_.bg = AnsiColor::indexed(static_cast<std::uint8_t>(code - 100 + 8));
    // 扩展色：`38;5;N` / `38;2;R;G;B`（背景同理 48）。
    if (code == 38 || code == 48) {
      AnsiColor color{};
      if (index + 1 < params_.size()) {
        const int kind = params_[index + 1];
        if (kind == 5 && index + 2 < params_.size()) {
          color = AnsiColor::indexed(static_cast<std::uint8_t>(std::clamp(params_[index + 2], 0, 255)));
          index += 2;
        } else if (kind == 2 && index + 4 < params_.size()) {
          color = AnsiColor::rgb(static_cast<std::uint8_t>(std::clamp(params_[index + 2], 0, 255)),
                                 static_cast<std::uint8_t>(std::clamp(params_[index + 3], 0, 255)),
                                 static_cast<std::uint8_t>(std::clamp(params_[index + 4], 0, 255)));
          index += 4;
        }
      }
      if (code == 38) style_.fg = color;
      else style_.bg = color;
    }
  }
}

void AnsiScreen::handle_csi(char final_byte) {
  // 私有模式（`CSI ? ... h/l`）：DEC 的开关走这条。
  if (private_marker_ && private_char_ == '?') {
    const bool on = final_byte == 'h';
    const bool off = final_byte == 'l';
    if (on || off) {
      for (const int value : params_) {
        switch (value) {
          case 25: cursor_visible_ = on; break;
          case 7: auto_wrap_ = on; break;
          case 1049:   // 切备用屏并保存/恢复光标（全屏程序的标准做法）
          case 47:
          case 1047: {
            if (on && !alt_active_) {
              // 进备用屏：主屏整份留着，备用屏从空白开始。
              saved_cursor_row_ = cursor_row_;
              saved_cursor_col_ = cursor_col_;
              std::swap(screen_, alt_screen_);
              for (auto& row : screen_) std::fill(row.begin(), row.end(), AnsiCell{});
              alt_active_ = true;
              cursor_row_ = 0;
              cursor_col_ = 0;
              mark_all_dirty();
            } else if (off && alt_active_) {
              std::swap(screen_, alt_screen_);   // 主屏原样回来
              alt_active_ = false;
              cursor_row_ = std::clamp(saved_cursor_row_, 0, rows_ - 1);
              cursor_col_ = std::clamp(saved_cursor_col_, 0, cols_ - 1);
              mark_all_dirty();
            }
            break;
          }
          default: break;   // 鼠标上报/括号粘贴等：**安静忽略**（见头文件说明）
        }
      }
    }
    params_.clear();
    param_has_value_ = false;
    private_marker_ = false;
    private_char_ = 0;
    return;
  }

  switch (final_byte) {
    case 'A': cursor_row_ = std::max(0, cursor_row_ - std::max(1, param(0, 1))); wrap_pending_ = false; break;
    case 'B': cursor_row_ = std::min(rows_ - 1, cursor_row_ + std::max(1, param(0, 1))); wrap_pending_ = false; break;
    case 'C': cursor_col_ = std::min(cols_ - 1, cursor_col_ + std::max(1, param(0, 1))); wrap_pending_ = false; break;
    case 'D': cursor_col_ = std::max(0, cursor_col_ - std::max(1, param(0, 1))); wrap_pending_ = false; break;
    case 'E':   // 下移 n 行并回行首
      cursor_row_ = std::min(rows_ - 1, cursor_row_ + std::max(1, param(0, 1)));
      cursor_col_ = 0;
      break;
    case 'F':
      cursor_row_ = std::max(0, cursor_row_ - std::max(1, param(0, 1)));
      cursor_col_ = 0;
      break;
    case 'G': cursor_col_ = std::clamp(param(0, 1) - 1, 0, cols_ - 1); break;
    case 'H':
    case 'f':   // 行列定位（1 基）
      cursor_row_ = std::clamp(param(0, 1) - 1, 0, rows_ - 1);
      cursor_col_ = std::clamp(param(1, 1) - 1, 0, cols_ - 1);
      wrap_pending_ = false;
      break;
    case 'd': cursor_row_ = std::clamp(param(0, 1) - 1, 0, rows_ - 1); break;
    case 'J': {   // 清屏
      const int mode = param(0, 0);
      if (mode == 0) {
        clear_row(cursor_row_, cursor_col_);
        for (int row = cursor_row_ + 1; row < rows_; ++row) clear_row(row, 0);
      } else if (mode == 1) {
        for (int row = 0; row < cursor_row_; ++row) clear_row(row, 0);
        auto& cells = line(cursor_row_);
        for (int col = 0; col <= std::min(cursor_col_, cols_ - 1); ++col) {
          cells[static_cast<std::size_t>(col)] = AnsiCell{};
        }
        set_dirty(cursor_row_);
      } else if (mode == 2 || mode == 3) {
        for (int row = 0; row < rows_; ++row) clear_row(row, 0);
        if (mode == 3) scrollback_.clear();
      }
      break;
    }
    case 'K': {   // 清行
      const int mode = param(0, 0);
      auto& cells = line(cursor_row_);
      if (mode == 0) {
        clear_row(cursor_row_, cursor_col_);
      } else if (mode == 1) {
        for (int col = 0; col <= std::min(cursor_col_, cols_ - 1); ++col) {
          cells[static_cast<std::size_t>(col)] = AnsiCell{};
        }
        set_dirty(cursor_row_);
      } else if (mode == 2) {
        clear_row(cursor_row_, 0);
      }
      break;
    }
    case 'X': {   // 清字符（不移动光标）
      const int count = std::max(1, param(0, 1));
      auto& cells = line(cursor_row_);
      for (int col = cursor_col_; col < std::min(cols_, cursor_col_ + count); ++col) {
        cells[static_cast<std::size_t>(col)] = AnsiCell{};
      }
      set_dirty(cursor_row_);
      break;
    }
    case 'L': {   // 在光标行插入 n 空行（滚动区域内）
      const int count = std::max(1, param(0, 1));
      if (cursor_row_ >= scroll_top_ && cursor_row_ <= scroll_bottom_) {
        scroll_down(cursor_row_, scroll_bottom_, count);
      }
      break;
    }
    case 'M': {   // 删 n 行
      const int count = std::max(1, param(0, 1));
      if (cursor_row_ >= scroll_top_ && cursor_row_ <= scroll_bottom_) {
        scroll_up(cursor_row_, scroll_bottom_, count);
      }
      break;
    }
    case 'P': {   // 删 n 字符
      auto& cells = line(cursor_row_);
      const int count = std::max(1, param(0, 1));
      // **宽字符感知的删除量**：xterm/kitty 的行为——删除点在宽字主格时，
      // 那个宽字（两格）算**一个**删除单位；末尾若被跨到的宽字只剩半张脸，
      // 也一并计入。纯格数删除会把宽字群成两半（实测：中文行删一格后整行错位）。
      int from = cursor_col_;
      if (from > 0 && from < cols_ && cells[static_cast<std::size_t>(from)].continuation) {
        --from;   // 删除点落在右半：从主格开始删（半张脸删不干净）
      }
      int width = 0;
      int steps = 0;
      while (steps < count && from + width < cols_) {
        const AnsiCell& cell_here = cells[static_cast<std::size_t>(from + width)];
        width += cell_here.continuation ? 1 : cell_width(cell_here.ch);
        ++steps;
      }
      // 末尾被跨到的半张宽字也计入（不留在行里成孤儿）。
      if (from + width < cols_ && cells[static_cast<std::size_t>(from + width)].continuation) {
        ++width;
      }
      const int limit = std::min(cols_, from + width);
      for (int col = from; col < cols_; ++col) {
        const int source = col + width;
        cells[static_cast<std::size_t>(col)] =
            source < cols_ ? cells[static_cast<std::size_t>(source)] : AnsiCell{};
      }
      (void)limit;
      set_dirty(cursor_row_);
      break;
    }
    case '@': {   // 插 n 空字符
      const int count = std::max(1, param(0, 1));
      auto& cells = line(cursor_row_);
      for (int col = cols_ - 1; col >= cursor_col_ + count; --col) {
        const int source = col - count;
        cells[static_cast<std::size_t>(col)] =
            source >= 0 ? cells[static_cast<std::size_t>(source)] : AnsiCell{};
      }
      for (int col = cursor_col_; col < std::min(cols_, cursor_col_ + count); ++col) {
        cells[static_cast<std::size_t>(col)] = AnsiCell{};
      }
      set_dirty(cursor_row_);
      break;
    }
    case 'S': scroll_up(scroll_top_, scroll_bottom_, std::max(1, param(0, 1))); break;
    case 'T': scroll_down(scroll_top_, scroll_bottom_, std::max(1, param(0, 1))); break;
    case 'b': {   // REP：重复上个字符 n 次（PowerShell 表格、图表艺术常用）
      if (last_graphic_ != 0) {
        const int count = std::max(1, param(0, 1));
        for (int i = 0; i < count; ++i) put(last_graphic_);
      }
      break;
    }
    case 'r':   // 设滚动区域（1 基）
      scroll_top_ = std::clamp(param(0, 1) - 1, 0, rows_ - 1);
      scroll_bottom_ = std::clamp(param(1, rows_) - 1, scroll_top_, rows_ - 1);
      cursor_row_ = scroll_top_;
      cursor_col_ = 0;
      break;
    case 's': store_row_ = cursor_row_; store_col_ = cursor_col_; store_style_ = style_; break;
    case 'u':
      cursor_row_ = std::clamp(store_row_, 0, rows_ - 1);
      cursor_col_ = std::clamp(store_col_, 0, cols_ - 1);
      style_ = store_style_;
      break;
    case 'm': apply_sgr(); break;
    case 'q': {   // 光标形状（DECSCUSR）
      const int code = param(0, 0);
      if (code == 3 || code == 4) cursor_shape_ = AnsiCursorShape::Underline;
      else if (code == 5 || code == 6) cursor_shape_ = AnsiCursorShape::Bar;
      else cursor_shape_ = AnsiCursorShape::Block;
      // 程序**明确指定**了形状 ⇒ 以后不再用宿主默认（否则 `vim` 设了竖线，
      // 宿主一改设置就把它覆盖回去）。
      shape_explicit_ = true;
      break;
    }
    case 'h':
    case 'l':
      break;   // 非私有模式的设置/复位：无对应状态，忽略
    default: break;   // 未知终止符：安静忽略（当文本画出来会污染屏幕）
  }

  params_.clear();
  param_has_value_ = false;
  private_marker_ = false;
  private_char_ = 0;
  intermediate_.clear();
}

void AnsiScreen::handle_escape(char byte) {
  switch (byte) {
    case '[': state_ = State::Csi; params_.clear(); param_has_value_ = false; break;
    case ']': state_ = State::Osc; osc_buffer_.clear(); break;
    case 'M':   // RI：上移一行（滚到顶就往下卷）
      if (cursor_row_ == scroll_top_) scroll_down(scroll_top_, scroll_bottom_, 1);
      else cursor_row_ = std::max(0, cursor_row_ - 1);
      break;
    case 'D':   // IND：下移一行
      if (cursor_row_ == scroll_bottom_) scroll_up(scroll_top_, scroll_bottom_, 1);
      else cursor_row_ = std::min(rows_ - 1, cursor_row_ + 1);
      break;
    case 'E':   // NEL：下移一行并回行首
      newline();
      cursor_col_ = 0;
      break;
    case '7': store_row_ = cursor_row_; store_col_ = cursor_col_; store_style_ = style_; break;
    case '8':
      cursor_row_ = std::clamp(store_row_, 0, rows_ - 1);
      cursor_col_ = std::clamp(store_col_, 0, cols_ - 1);
      style_ = store_style_;
      break;
    case 'c':   // RIS：整机复位
      for (auto& row : screen_) std::fill(row.begin(), row.end(), AnsiCell{});
      cursor_row_ = 0;
      cursor_col_ = 0;
      reset_style();
      cursor_visible_ = true;
      auto_wrap_ = true;
      scroll_top_ = 0;
      scroll_bottom_ = rows_ - 1;
      mark_all_dirty();
      break;
    case '(':
    case ')':
    case '*':
    case '+': state_ = State::Charset; break;   // 字符集选择：本层不做（忽略一个字节）
    case '=':
    case '>':
      break;   // 键盘模式切换（应用键盘/数字键盘）：不影响屏幕
    default: break;
  }
}

void AnsiScreen::handle_osc(std::string_view payload) {
  // 只认窗口标题（`0;title` / `2;title`）；其余 OSC（超链接、剪贴板、进度）
  // 安静忽略——它们要宿主配合，本层不做。
  const std::size_t semicolon = payload.find(';');
  if (semicolon == std::string_view::npos) return;
  const std::string_view code = payload.substr(0, semicolon);
  if (code == "0" || code == "2") {
    title_ = std::string(payload.substr(semicolon + 1));
  }
}

void AnsiScreen::feed(std::string_view bytes) {
  // 宿主默认光标形状：**只在屏幕内容没说话时**生效。
  //
  // 为什么放在这里：用户可以在运行期改内置设置（默认形状），而 PTY 是常驻的
  //——属性改动没有“重建屏幕”的机会。本层是唯一知道“程序是否已用 `DECSCUSR`
  // 表过态”的地方，所以在每次喂字节前把它保守地覆盖一遍：
  // 已经 explicit 的会话**不受影响**（`set_default_cursor_shape` 里的守卫）。
  if (default_cursor_shape_.has_value()) set_default_cursor_shape(*default_cursor_shape_);
  // UTF-8 残片先接上（PTY 分片边界与码点边界无关，这一步不能省）。
  std::string input;
  if (!utf8_pending_.empty()) {
    input = std::move(utf8_pending_);
    utf8_pending_.clear();
  }
  input.append(bytes);

  for (std::size_t index = 0; index < input.size();) {
    const auto byte = static_cast<unsigned char>(input[index]);
    switch (state_) {
      case State::Ground: {
        if (byte == 0x1BU) {   // ESC
          state_ = State::Escape;
          ++index;
          continue;
        }
        if (byte < 0x20U || byte == 0x7FU) {   // C0 控制字符
          switch (byte) {
            case '\n': case '\v': case '\f': newline(); break;
            case '\r': carriage_return(); break;
            case '\t': tab(); break;
            case '\b': backspace(); break;
            case 0x07: ++bell_count_; break;   // BEL：只计数
            default: break;                    // 其余 C0 忽略
          }
          ++index;
          continue;
        }
        // 文本（可能是多字节）
        const int length = utf8_length(byte);
        if (static_cast<std::size_t>(length) > input.size() - index) {
          // 码点被切断：留到下一次 `feed`。
          utf8_pending_ = input.substr(index);
          index = input.size();
          continue;
        }
        put(utf8_decode(std::string_view(input).substr(index, static_cast<std::size_t>(length))));
        index += static_cast<std::size_t>(length);
        continue;
      }
      case State::Escape:
        state_ = State::Ground;
        handle_escape(static_cast<char>(byte));
        ++index;
        continue;
      case State::Charset:
        state_ = State::Ground;   // 吃掉字符集标识符那一个字节
        ++index;
        continue;
      case State::Csi: {
        if ((byte >= '0' && byte <= '9') || byte == ';' || byte == ':') {
          // 参数（`:` 是子参数分隔符，这里当 `;` 处理——本层没有用到子参数）
          if (byte >= '0' && byte <= '9') {
            // 只在此参数还没开始时开新槽；一旦开始就继续累加。
            if (!param_has_value_) {
              params_.push_back(0);
              param_has_value_ = true;
            }
            params_.back() = params_.back() * 10 + (byte - '0');
          } else {
            // `;` 分隔符：
            // * 上一个参数已结束 → 只把标志落下（**不要**再推占位，
            //   否则 `\x1b[1;31m` 会变成 `1,-1` 而丢掉 `31`，字不上色）；
            // * 上一个参数是**空的**（如 `\x1b[;5H` 的第一个空参）→ 推 -1 占位，
            //   否则后面的参数会整体左移一格。
            if (!param_has_value_) params_.push_back(-1);
            param_has_value_ = false;
          }
          ++index;
          continue;
        }
        if (byte == '?' || byte == '>' || byte == '<' || byte == '=') {
          private_marker_ = true;
          private_char_ = static_cast<char>(byte);
          params_.clear();
          param_has_value_ = false;
          ++index;
          continue;
        }
        if (byte >= 0x20U && byte < 0x30U) {   // 中间字节（`"` `$` ` ` 等）
          intermediate_.push_back(static_cast<char>(byte));
          ++index;
          continue;
        }
        // 终止符（0x40-0x7E）
        state_ = State::Ground;
        if (byte >= 0x40U && byte <= 0x7EU) {
          handle_csi(static_cast<char>(byte));
        }
        ++index;
        continue;
      }
      case State::Osc: {
        if (byte == 0x07U) {   // BEL 收尾
          state_ = State::Ground;
          handle_osc(osc_buffer_);
          osc_buffer_.clear();
          ++index;
          continue;
        }
        if (byte == 0x1BU) {   // ST 的前半
          state_ = State::OscEscape;
          ++index;
          continue;
        }
        osc_buffer_.push_back(static_cast<char>(byte));
        ++index;
        continue;
      }
      case State::OscEscape:
        // `ESC \` = ST（字符串终止符）；其它（如 `ESC [`）不是合法 OSC 结束——
        // 别把后续字节无端吞掉（实测：vim 设光标色 `OSC 12 ; ...` 后跟 `CSI`
        // 时会把 CSI 的首字节吃掉，后面整个序列错位）。裸 `ESC` 本身也**不是**
        // payload 的一部分（否则标题里会多出 0x1B）——丢弃它、回到 OSC 继续。
        if (byte == '\\') {
          state_ = State::Ground;
          handle_osc(osc_buffer_);
          osc_buffer_.clear();
        } else if (byte == 0x1BU) {
          state_ = State::OscEscape;   // 又一个 ESC：继续等（罕见，不丢状态）
        } else {
          osc_buffer_.push_back(static_cast<char>(byte));
          state_ = State::Osc;
        }
        ++index;
        continue;
    }
  }
}

}  // namespace st::text
