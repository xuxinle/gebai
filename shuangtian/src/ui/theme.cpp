#include "st/ui/theme.hpp"

#include <algorithm>
#include <iterator>

namespace st::ui {
namespace {

// 设计令牌一律用 `from_rgba_hex`（显式 0xRRGGBBAA）：
// `from_hex` 靠数值大小猜格式，遇到“高位字节为 0 且带 alpha”的字面量会静默误读——
// 实测 `0x0000008C`（黑色 55%）被当成 `0x00008C`（纯蓝、不透明），
// 结果是深色主题的卡片阴影发蓝光。字面量都写成 8 位 + 显式 alpha 就没有这个歧义。
[[nodiscard]] auto hex(std::uint32_t value) -> math::Color {
  return math::Color::from_rgba_hex(value);
}

}  // namespace

auto Theme::light() -> Theme {
  Theme theme;
  theme.mode_ = ThemeMode::Light;
  theme.name_ = "light";
  Palette& palette = theme.colors_;
  // —— 白色亚克力（默认亮色）——
  //
  // ## 表面阶梅（2026-10-18 重定，与暗色同构）
  //
  // 旧阶梯的问题（用户报“编辑器不够下沉”）：`bg`/`surface`/`sunken` 三档里
  // 编辑器落在 `bg` 上，而终端面板落在 `surface` 上——结果是编辑器比终端**更暗**，
  // 与“内容区应当最沉”的直觉相反，且侧栏与编辑器完全同色（1.000）。
  //
  // 新阶梯（对齐 VSCode Light+ / IDEA Light）：
  //   外壳条带（标题/活动/状态栏） → `surface_alt`（中性浅灰）
  //   侧栏/面板               → `surface`（近白）
  //   编辑器/终端              → `surface_sunken`（微灰，往里沉）
  //   输入框/代码块            → 再沉一档（`code_bg` / 控件自己的 `sunken`）
  //   弹层/菜单               → `surface_raised`（纯白 + 阴影）
  // 页面底：与面板拉得开（护栏：`surface` 对 `bg` ≥1.08）。
  // —— 白色亚克力（默认亮色，对齐歌白前端 `themes/acrylic.css`）——
  //
  // ## 色相取歌白，**层差按霜天要求拉开**
  //
  // 歌白的亚克力是**半透明叠层 + backdrop-filter 毛玻璃**：它的面板与页底
  // 预合成后只差 1.03（`#FEFEFE` vs `#FAFAFC`），层差靠模糊与阴影补足。
  // 霜天自绘管线没有 backdrop-filter（DESIGN §5），**层差必须自己给足**，
  // 否则 `ui_theme_test` 的相邻 ≥1.08 过不了，界面也会糊成一片。
  //
  // 所以：色相与冷暖基调取歌白（**冷中性**，不是暖中性），明度按最小可辨拉开。
  //
  //   页底 `#F4F5F9`   面板 `#FEFEFF`   外壳/次面 `#F7F7FA`   凹槽 `#E9EAF0`
  //   实测：面板/页底 1.081（要求 ≥1.08）· 面板/次面 1.061（≥1.05）· 凹槽/页底 1.140
  palette.bg = hex(0xF4F5F9FFU);
  palette.surface = hex(0xFEFEFFFFU);
  // 外壳条带：压暗一点（亮色下“更亮”已到头，靠压暗边框区把内容托起来）。
  // VSCode Light：侧栏 #f3f3f3 / 编辑器 #ffffff；本主题同构。
  palette.surface_alt = hex(0xF7F7FAFFU);
  // 凹槽（输入框底/代码块/进度槽）：**真凹进去**（比 `bg` 暗）——
  // 它不服务于编辑器（编辑器走 `Surface::Content`，见下）。
  palette.surface_sunken = hex(0xE9EAF0FFU);
  // 编辑内容区 / 抬升面（同一个 token 服务两个语义）：
  //   · `Surface::Content`（编辑区阅读面）亮色下就是**纯白**——与外壳（`alt` 浅灰）拉开一层；
  //   · 弹层/菜单（`surface_raised`）浅色下也是白系，抬升由**阴影**表达（见 `elevation`）。
  // 两者取值相同是有意的：亮色下“最亮的面”只有一个候选值。
  palette.surface_raised = hex(0xFFFFFFFFU);
  // 描边是**半透明黑光**（`theme.hpp` 亚克力口径 §1）：叠在任意底色上强度一致。
  //
  // 为何不能用“按主体底色合成一次的等效实色”：那只能对**一种**底色正确。
  // 实测等效实色 `#D2D2D9` 的强度在四个底色上是 1.10~1.44（**离散 0.339**），
  // 正是 `theme.hpp` 说的“在卡片上清楚、在页底上糊掉”。
  // 改成 `rgba(0,0,0,.11)` 后：1.27~1.28（**离散 0.006**），且平均强度不变。
  palette.border = hex(0x0000001AU);         // rgba(0,0,0,26/255)  合成 1.276
  palette.border_strong = hex(0x00000030U);  // rgba(0,0,0,48/255)  合成 1.552
  // 细分隔线：**比 `border` 更淡**（护栏：`subtle` 强度 < `border`）——
  // 它的职责是“同组内”的弱分隔，不能抢过控件边界。
  // 半透明后两个条件同时满足：平均 1.16 < 1.27，且对每个底色都 ≥ 1.15。
  palette.border_subtle = hex(0x0000000FU);  // rgba(0,0,0,15/255)  合成 1.152
  palette.text = hex(0x141418FFU);
  palette.text_muted = hex(0x4E4E5AFFU);
  // `faint` 会被用在**编辑器内**（注释/行号）——而编辑器底是 `sunken`（阶梯最低点）。
  // 它必须同时对 `bg`/`alt`/`surface`/`sunken` 达 4.5:1（WCAG AA 正文口径）。
  // 四个底色的实测值（`sunken` 是最严的一条，它最亮、与文字差最小）：
  //   bg 4.96 / surface 5.37 / alt 5.06 / **sunken 4.51**
  // `#696976` 而不是歌白的 `#7A7A88`：后者对凹槽只有 4.20，不足 AA 正文口径。
  palette.text_faint = hex(0x696976FFU);
  // 主色取歌白 `accent` 与 `accent-hover` 之间：歌白的 `#5566EE` 对白面只有 4.61（无余量），
  // 而且它的"主色底上白字"也只有 4.65——霜天两个方向都要 ≥4.5，留不出安全边界。
  palette.primary = hex(0x4A5AE0FFU);
  palette.primary_hover = hex(0x3E4DD0FFU);
  palette.primary_active = hex(0x3642BEFFU);
  // 悬浮档位要**看得见**：早先取 0xF2F6FC，与侧栏底色只差 4/2/0（肉眼近乎没有），
  // 实测像素差才 6/255。现在这一步在浅色底上清晰可辨，又不至于喧宾夺主。
  palette.surface_hover = hex(0xF0F1F6FFU);
  palette.surface_pressed = hex(0xE2E4EDFFU);
  palette.border_hover = hex(0x9A9AA6FFU);
  palette.glow = hex(0x4A5AE02EU);
  palette.primary_soft = hex(0xEDEEFBFFU);
  // 顶部内高光：**浅色档留给透明**。
  //
  // 亚克力的"玻璃边缘"在两个模式下不是同一件事：深色底的边缘是**弱白光**（提亮），
  // 而浅色底已经接近纯白，再叠白光不会可见——歌白浅色档用的是一条**暗色发丝线**
  // （`--shadow-glow: 0 0 0 1px rgba(0,0,0,.06)`），而那正是 `border` 已经承担的职责。
  // 所以浅色档不再重复一层：靠 `border` + 阴影表达边缘，深色档才用本令牌。
  palette.highlight = hex(0xFFFFFF00U);
  palette.on_primary = hex(0xFFFFFFFFU);
  palette.accent = hex(0x0E849CFFU);
  palette.accent_soft = hex(0xE2EEF2FFU);
  palette.success = hex(0x1F9D5FFFU);
  palette.warning = hex(0xB07F1EFFU);
  palette.danger = hex(0xD63A40FFU);
  palette.focus_ring = hex(0x4A5AE066U);
  palette.overlay = hex(0x0A0A0F4DU);
  // 阴影基色 alpha 定在 0x26（15%）：`shadow_lg` 的关键层乘 1.05 → ≈16%，
  // 刚好卡在"浅底上单层不超过 16% 否则显脏"的上限内。
  palette.shadow = hex(0x0A0A1226U);
  palette.selection = hex(0xC6CBF7FFU);
  palette.code_bg = hex(0xF2F3F7FFU);
  palette.code_border = hex(0xE4E5ECFFU);
  // 语法色板（浅色）：关键字紫、字符串绿、数字橙、注释灰、类型青、函数蓝——彼此高辨识度
  theme.syntax_.plain = hex(0x1E2433FFU);
  theme.syntax_.keyword = hex(0x8B31C7FFU);
  theme.syntax_.type = hex(0x0E7C86FFU);
  theme.syntax_.string = hex(0x1A7F4BFFU);
  theme.syntax_.number = hex(0xB45309FFU);
  theme.syntax_.comment = hex(0x8A94A6FFU);
  theme.syntax_.function = hex(0x3B4BC8FFU);
  theme.syntax_.operator_ = hex(0x64748BFFU);
  theme.syntax_.punctuation = hex(0x94A3B8FFU);
  theme.syntax_.preprocessor = hex(0xC2410CFFU);
  theme.syntax_.builtin = hex(0x0F766EFFU);
  theme.syntax_.attribute = hex(0xBE185DFFU);
  theme.syntax_.key = hex(0x2E3AB4FFU);
  theme.syntax_.tag = hex(0xB91C1CFFU);
  theme.syntax_.inserted = hex(0x15803DFFU);
  theme.syntax_.deleted = hex(0xB91C1CFFU);
  theme.syntax_.line_number = hex(0xA0AEC0FFU);
  theme.syntax_.current_line = hex(0x4655D80AU);
  theme.syntax_.selection = hex(0x4655D833U);
  theme.syntax_.find_highlight = hex(0xF5D76E66U);  // 黄系：全部命中（VSCode 同族）
  theme.syntax_.find_active = hex(0xF0A72EFFU);     // 当前命中更深
  theme.syntax_.cursor = hex(0x4655D8FFU);
  theme.syntax_.matching_bracket = hex(0xF59E0B55U);
  // 终端色板（浅色）：**白底 + 暗色 16 色**。
  //
  // 为何不能沿用深底那套：ANSI 色是**协议固定语义**（`31` 永远是红），
  // 但它的**明度必须跟底色走**——为深底调的亮黄 `#E5E510` 放到白底上
  // 对比度只有 1.00（字面看不见）、亮白 `#E5E5E5` 是 1.07、亮绿 `#23D18B` 是 1.47。
  // 这里每个色都对白底 ≥6.2:1（回归断言锁 ≥4.5，即 WCAG AA 正文口径）。
  theme.terminal_.bg = hex(0xFFFFFFFFU);
  theme.terminal_.fg = hex(0x1F2430FFU);
  theme.terminal_.cursor = hex(0x4655D8FFU);
  theme.terminal_.selection = hex(0x4655D833U);
  theme.terminal_.ansi = {
      hex(0x3A3A45FFU), hex(0xB32218FFU), hex(0x0A6234FFU), hex(0x7A5600FFU),
      hex(0x1A46C4FFU), hex(0x861F9BFFU), hex(0x0B6477FFU), hex(0x57616FFFU),
      hex(0x4E4E5CFFU), hex(0xBC1C12FFU), hex(0x0E6F3CFFU), hex(0x865700FFU),
      hex(0x1D4ED8FFU), hex(0x9317A8FFU), hex(0x096B80FFU), hex(0x3A3A45FFU)};
  theme.metrics_ = Metrics{};
  // 凹槽内阴影（亮色）：一道**极淡的灰**。
  //
  // 为何这么淡：`surface_sunken` 已经比周围深一档（#DCDCE5 vs #FAFAFC），
  // 内阴影只负责把“凹进去”的**方向**说出来（上缘略暗、下缘略亮），
  // 再重就会被读成“脏边”——与卡片边框同一个道理（见 DESIGN §4.2.4）。
  theme.metrics_.sunken_shadow = hex(0x00000026U);   // ~15% 黑
  theme.metrics_.sunken_shadow_blur = 5.0f;
  // 弹层毛玻璃：只给弹层（菜单/对话框/命令面板/气泡）开。
  // 半径 14（逻辑 px）：大到能抹平背后的文字与图标细节，又不会把颜色也糊成一片。
  //
  // ## 为何亮色档的着色不透明度要**高**（0.88 而不是 0.72）
  //
  // 弹层绘在**遮罩**之上，而亮色遮罩是 `rgba(10,10,15,0.3)`——把整屏压暗了。
  // 模糊只能让这块背板“糊”掉，**不会让它变亮**：0.72 的着色意味着
  // 还有 28% 的暗遮罩透上来，弹层于是比背后还暗。
  // 实测（亮色对话框）：弹层卡面亮度 **0.168**、背后遮罩区 **0.457**，
  // 比值 0.37——弹层看起来是“陷进去”的，只有主色按钮显眼。
  // 亚克力的“玻璃”是**带着自己的底色**浮在上面的，模糊只贡献质感，不该决定亮度。
  theme.metrics_.overlay_blur_radius = 14.0f;
  theme.metrics_.overlay_blur_tint = 0.88f;
  return theme;
}

auto Theme::dark() -> Theme {
  Theme theme;
  theme.mode_ = ThemeMode::Dark;
  theme.name_ = "dark";
  Palette& palette = theme.colors_;
  // —— 黑色亚克力（默认暗色）——
  //
  // ## 表面阶梅（2026-10-18 重定，对齐 VSCode Dark+ / IDEA Darcula）
  //
  // 旧阶梯的问题（用户报“编辑器背景不够下沉、与 UI 无区分度”）：
  // `bg` 与 `sunken` 都很深，而**编辑器落在 `bg` 上**——于是编辑器、侧栏、
  // 标签条全是同一个色（对比度 1.000），屏幕上一片糊；资源管理器反而更亮，
  // 形成“内亮外暗”的颠倒。
  //
  // 主流的层次不是“越大越亮”，而是：
  //   外壳条带（标题/活动/状态栏）  —— **最亮**，它是“框”
  //   侧栏/面板（内容容器）      —— 中间
  //   编辑器（焦点内容）        —— **比外壳暗**，往里沉
  //   输入框/代码块/凹槽        —— 再暗一档
  //   弹层/菜单            —— 亮回来（靠“浮起”脱离）
  //
  // 实测参照（VSCode Dark+）：编辑器 #1e1e1e < 侧栏 #252526 < 外壳 #333333。
  // 本主题按同一方向排，色相保持“中性极低饱和度”的亚克力口径不变。
  // 页面底。
  // 它是暗色阶梯的**最低点**（`sunken` 必须比它亮——见 `ui_theme_test`），
  // 所以不能像旧值那样比 `sunken` 还亮。同时它参与 `faint` 文字对比度取最小值，
  // 压得太黑会把 `text_faint` 的 4.0:1 拖下去（实测 #0A0A0C 就不过）。
  // —— 黑色亚克力（默认暗色，对齐歌白前端 `themes/acrylic.css`）——
  //
  // 色相取歌白（**冷中性**：`#0B0B0E` 略带冷调，不是偏蓝），层差按霜天要求拉开：
  // 歌白预合成后 面板/页底 只差 1.04，靠毛玻璃补；霜天无毛玻璃，必须自己给到 ≥1.08。
  //
  //   页底 `#0B0B0E`   面板 `#15151A`   外壳/次面 `#1E1E26`   凹槽 `#07070A`   抬升面 `#25252E`
  //   实测：面板/页底 1.080 · 次面/面板 1.100 · 凹槽/面板 1.106 · 抬升面 ≥ 次面
  palette.bg = hex(0x0B0B0EFFU);
  palette.surface = hex(0x15151AFFU);      ///< 侧栏/面板：内容容器
  palette.surface_alt = hex(0x1F1F28FFU);  ///< 外壳条带：比面板再亮一档
  palette.surface_sunken = hex(0x07070AFFU); ///< 编辑器底/输入框/代码块：往内沉
  // 抬升面（弹层/菜单）比 surface 再亮一档：深色下**阴影几乎不可见**，
  // 层次只能靠底色本身拉开。
  palette.surface_raised = hex(0x25252EFFU);
  // 描边 = 半透明白光叠在**面板色**上的等效值。
  // 三档必须**同时**满足两组朝向相反的约束（`ui_theme_test` 锁着）：
  //   ● 各自对 `surface`/`alt` 看得见（≥ 1.10）——外壳条带抬高后旧值集体不够；
  //   ● 亮度顺序 `subtle < border < border_strong`——深色下“更淡 = 更暗”。
  //
  // 与浅色同理：**用半透明白光**，不用不透明实色（`theme.hpp` 亚克力口径 §1）。
  // 不透明实色在四个底色上的强度离散度实测 0.43~0.89，正是“一处清楚一处糊”。
  // 半透明后离散度降到 0.06~0.10，且平均强度与原设计一致（2.05 / 2.84 / 1.37）。
  palette.border = hex(0xFFFFFF1FU);         // rgba(255,255,255,31/255)  合成 1.415
  // `border_strong` 的 alpha **被一条结构性约束卡住**，不是随手取的数：
  // `ui_theme_test` 要求描边族在四个底色上的强度**离散度 ≤ 0.15**，
  // 而暗色四底色的明度跨度（sunken 比 surface 暗 1.11、alt 比 surface 亮 1.11）
  // 本身就决定了同一道白光的离散度有一个随 alpha 单调上升的下限：
  //   alpha 34 → 0.1504（超）  alpha 33 → 0.1451  alpha 32 → 0.1397（取此）
  // 所以取 32：它给量化误差留了 ~0.01 的余量（离散度按**逐通道取整后**的值算，
  // 浮点直算会少算 ~0.004——实测就是这个差把 alpha 34 误判成"能过"）。
  palette.border_strong = hex(0xFFFFFF20U);  // rgba(255,255,255,32/255)  合成 1.427
  palette.border_subtle = hex(0xFFFFFF12U);  // rgba(255,255,255,18/255)  合成 1.220
  palette.text = hex(0xF2F2F5FFU);
  palette.text_muted = hex(0xB4B4BEFFU);
  // `faint` 会在**外壳条带（`alt`）上**用（状态栏提示/行号），而 `alt` 本次抬高了一档；
  // 旧值 #80808C 对 `alt` 只有 3.64，不够 4.0。抬到对三档均 ≥ 4.0。
  // 取歌白 `#80808C`：对四个底色的最小值 4.62（要求 4.0）。
  palette.text_faint = hex(0x80808CFFU);
  palette.primary = hex(0x8B9AFFFFU);
  palette.primary_hover = hex(0xA0ADFFFFU);
  palette.primary_active = hex(0x7787F5FFU);
  // 深色主题同样要给这套令牌：漏掉的话默认是全透明，**悬浮在暗色下毫无反馈**——
  // 而这种"某个主题下静默失效"的问题最难发现（浅色看着一切正常）。
  palette.surface_hover = hex(0x1C1C24FFU);
  palette.surface_pressed = hex(0x24242EFFU);
  palette.border_hover = hex(0x70707EFFU);
  palette.glow = hex(0x8B9AFF38U);
  palette.primary_soft = hex(0x23263AFFU);
  palette.highlight = hex(0x2E2E38FFU);   ///< 顶部内高光：深色下的玻璃边缘（弱白光）
  palette.on_primary = hex(0x0B0B14FFU);
  palette.accent = hex(0x6FD8E8FFU);
  palette.accent_soft = hex(0x1C2A2EFFU);
  palette.success = hex(0x55D396FFU);
  palette.warning = hex(0xE6BD6DFFU);
  palette.danger = hex(0xFF7A7AFFU);
  palette.focus_ring = hex(0x8B9AFF66U);
  palette.overlay = hex(0x000000A6U);
  // 深色主题的阴影基色比浅色重得多：深底上投影几乎看不见，需要更高不透明度
  // 才能提供一点“离地”分离度。
  palette.shadow = hex(0x000000B3U);
  palette.selection = hex(0x2B3D66FFU);
  // 代码块/行内代码底：与“编辑器底”再拉开一档（代码块是嵌在正文里的凹槽）。
  palette.code_bg = hex(0x0D0D11FFU);
  palette.code_border = hex(0x26262EFFU);
  // 语法色板（深色）：同族色相提亮，保证在深底上仍有足够对比
  theme.syntax_.plain = hex(0xD7E0F0FFU);
  theme.syntax_.keyword = hex(0xC792EAFFU);
  theme.syntax_.type = hex(0x7FDBFFFFU);
  theme.syntax_.string = hex(0xA5E075FFU);
  theme.syntax_.number = hex(0xF7A85FFFU);
  theme.syntax_.comment = hex(0x6B7A90FFU);
  theme.syntax_.function = hex(0x93A1FFFFU);
  theme.syntax_.operator_ = hex(0x9AA9C0FFU);
  theme.syntax_.punctuation = hex(0x7C8AA3FFU);
  theme.syntax_.preprocessor = hex(0xFFB07BFFU);
  theme.syntax_.builtin = hex(0x7FE3D4FFU);
  theme.syntax_.attribute = hex(0xFF9ED2FFU);
  theme.syntax_.key = hex(0xA0ADFFFFU);
  theme.syntax_.tag = hex(0xFF8F8FFFU);
  theme.syntax_.inserted = hex(0x6EE7A0FFU);
  theme.syntax_.deleted = hex(0xFF9A9AFFU);
  theme.syntax_.line_number = hex(0x54637AFFU);
  theme.syntax_.current_line = hex(0x8B9AFF1AU);
  theme.syntax_.selection = hex(0x8B9AFF44U);
  theme.syntax_.find_highlight = hex(0x6B5B1E99U);  // 暗色下的黄系命中
  theme.syntax_.find_active = hex(0x9A7B1FFFU);
  theme.syntax_.cursor = hex(0xB8C4FFFFU);
  theme.syntax_.matching_bracket = hex(0xE6BD6D55U);
  // 终端色板（深色）：经典终端黑 `#0C0C0C` + 亮 16 色。
  // 底取经典黑而不是 `surface_sunken`（`#050507`）：后者是**通用凹槽语义**
  //（输入框/代码底共用），奇黑偏沉；`#0C0C0C` 才是“一块屏幕”应当的观感。
  // 每个色对底 ≥5.2:1（回归断言锁 ≥4.5；black/brBlack 为灰阶，不参与）。
  theme.terminal_.bg = hex(0x0C0C0CU);
  theme.terminal_.fg = hex(0xE6E6E6FFU);
  theme.terminal_.cursor = hex(0x8B9AFFFFU);
  theme.terminal_.selection = hex(0x8B9AFF44U);
  theme.terminal_.ansi = {
      hex(0x3A3A45FFU), hex(0xE05555FFU), hex(0x0DBC79FFU), hex(0xE5E510FFU),
      hex(0x4A8FE0FFU), hex(0xCE63CEFFU), hex(0x11A8CDFFU), hex(0xE5E5E5FFU),
      hex(0x8A8A8AFFU), hex(0xF14C4CFFU), hex(0x23D18BFFU), hex(0xF5F543FFU),
      hex(0x3B8EEAFFU), hex(0xD670D6FFU), hex(0x29B8DBFFU), hex(0xFFFFFFFFU)};
  theme.metrics_ = Metrics{};
  // 凹槽内阴影（暗色）：比亮色**稍强**。
  //
  // 暗底上一切都很淡：同样的 alpha 在暗色里几乎读不出方向（实测 15% 黑在
  // #0D0D10 上只压出 2/255），所以这里提到 ~26%。
  theme.metrics_.sunken_shadow = hex(0x00000042U);   // ~26% 黑
  theme.metrics_.sunken_shadow_blur = 6.0f;
  // 弹层毛玻璃（暗色）：半径与亮色同。
  //
  // 暗色着色 0.82（比亮色的 0.88 略低）：暗底上背板本来就暗，透上来一点
  // 不会把弹层压暗，而透得多一些才能看出“背后有东西”（亚克力的意思就在这里）。
  // 但**不能低到让弹层比背板暗**——那是同一类缺陷，只是更容易在暗色里发生。
  theme.metrics_.overlay_blur_radius = 14.0f;
  theme.metrics_.overlay_blur_tint = 0.82f;
  return theme;
}

auto Theme::by_mode(ThemeMode mode) -> Theme { return mode == ThemeMode::Dark ? dark() : light(); }

auto tone_color(const Theme& theme, Tone tone) -> math::Color {
  const Palette& palette = theme.colors();
  switch (tone) {
    case Tone::Default: return palette.text;
    case Tone::Muted: return palette.text_muted;
    case Tone::Faint: return palette.text_faint;
    case Tone::Primary: return palette.primary;
    case Tone::Accent: return palette.accent;
    case Tone::Success: return palette.success;
    case Tone::Warning: return palette.warning;
    case Tone::Danger: return palette.danger;
    case Tone::OnPrimary: return palette.on_primary;
  }
  return palette.text;
}

auto tone_soft_color(const Theme& theme, Tone tone) -> math::Color {
  const Palette& palette = theme.colors();
  switch (tone) {
    case Tone::Primary: return palette.primary_soft;
    case Tone::Accent: return palette.accent_soft;
    case Tone::Success: return palette.success.with_alpha_f(0.14f);
    case Tone::Warning: return palette.warning.with_alpha_f(0.16f);
    case Tone::Danger: return palette.danger.with_alpha_f(0.14f);
    case Tone::Default:
    case Tone::Muted:
    case Tone::Faint:
    case Tone::OnPrimary: return theme.mode() == ThemeMode::Dark ? palette.surface_alt
                                                                : palette.surface_alt;
  }
  return palette.surface_alt;
}

/// 阴影三档（均为“关键光 + 环境光”两层，基色取主题的 `colors().shadow`，可被主题覆盖）。
///
/// 参数取值原则：
/// - **关键层**紧（blur 小、偏移 1~4px）：提供“边缘与底面接触”的可信感；
/// - **环境层**大（blur 是关键层的 3~4 倍、更淡）：提供“离地高度”的纵深感；
/// - 浅色底上总不透明度控制在 20% 以内：超过会显脏（像污渍而不是光）；
/// - 深色底上阴影几乎看不见，层次靠 `border`/`surface_alt`——阴影只补一点分离度。
///
/// ⚠ 返回的 `color.a` **不受 `shadow_strength` 影响**，只改模糊半径与偏移？——不：两者都受控。
/// 因为 `ui_theme_test.cpp` 的“单层不透明度 ≤ 16%”契约是对**默认档**的断言，
/// 所以强度旋钮作用在**基色透明度**上，而默认值下各层结果与旧值一致。
namespace {

/// 从基色按比例派生一层（`factor` 为相对基色不透明度的倍数）。
[[nodiscard]] auto shadow_layer(const math::Color& base, float factor) -> math::Color {
  return base.with_alpha_f(static_cast<float>(base.a) / 255.0f * factor);
}

/// 应用主题的强度/扩散旋钮：强度乘在透明度上，扩散乘在模糊与偏移上。
/// 非正值忽略（宁可保留默认，也不要静默把阴影变成 0 或负偏移）。
void apply_shadow_tuning(const Theme& theme, Shadow& shadow) {
  const Metrics& metrics = theme.metrics();
  if (metrics.shadow_strength > 0.0f && metrics.shadow_strength != 1.0f) {
    shadow.color = shadow.color.scale_alpha(metrics.shadow_strength);
    shadow.color2 = shadow.color2.scale_alpha(metrics.shadow_strength);
  }
  if (metrics.shadow_spread > 0.0f && metrics.shadow_spread != 1.0f) {
    shadow.blur *= metrics.shadow_spread;
    shadow.blur2 *= metrics.shadow_spread;
  }
}

}  // namespace

[[nodiscard]] auto shadow_sm(const Theme& theme) -> Shadow {
  const math::Color base = theme.colors().shadow;
  Shadow shadow;
  shadow.color = shadow_layer(base, 0.90f);
  shadow.blur = 3.0f;
  shadow.offset_y = 1.0f;
  shadow.color2 = shadow_layer(base, 0.60f);
  shadow.blur2 = 10.0f;
  shadow.offset2_y = 3.0f;
  apply_shadow_tuning(theme, shadow);
  return shadow;
}

[[nodiscard]] auto shadow_md(const Theme& theme) -> Shadow {
  const math::Color base = theme.colors().shadow;
  Shadow shadow;
  shadow.color = base;
  shadow.blur = 6.0f;
  shadow.offset_y = 2.0f;
  shadow.color2 = shadow_layer(base, 0.70f);
  shadow.blur2 = 22.0f;
  shadow.offset2_y = 6.0f;
  apply_shadow_tuning(theme, shadow);
  return shadow;
}

[[nodiscard]] auto shadow_lg(const Theme& theme) -> Shadow {
  const math::Color base = theme.colors().shadow;
  Shadow shadow;
  shadow.color = shadow_layer(base, 1.05f);
  shadow.blur = 10.0f;
  shadow.offset_y = 4.0f;
  shadow.color2 = shadow_layer(base, 0.85f);
  shadow.blur2 = 40.0f;
  shadow.offset2_y = 14.0f;
  apply_shadow_tuning(theme, shadow);
  return shadow;
}

auto ease_out(float t, float power) noexcept -> float {
  // 夹到 [0,1]：动画首尾会因为时间戳抖动给出 −1e-7 这类值,
  // 而 `pow(负数, 非整数次)` 是 NaN——NaN 混进布局会变成“元素凭空消失”。
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  // `power <= 0` 退化成线性（而不是让整体变成 0：那会让动画卡在起点）。
  if (!(power > 0.0f)) return clamped;
  // `power == 1` 时 `pow` 与直减等价，但省一次 `pow` 调用（每帧每控件都在算）。
  if (power == 1.0f) return clamped;
  return 1.0f - std::pow(1.0f - clamped, power);
}

auto ease_out(const Theme& theme, float t) noexcept -> float {
  return ease_out(t, theme.metrics().motion_ease_out_power);
}

// —— 令牌名表 ——
//
// 实现方式是"名字数组 + 一个取址表"，而不是两张 switch：
// 前者能保证"名字清单"与"按名取址"**不会漂移**（两者共用同一份列表），
// 而手写的 switch 很容易新增 token 时漏掉、且编译器不会提醒。
namespace {

/// 每个 token 一个 `(名字, 成员指针)` 对。成员指针让三个入口
/// （名表 / 取色 / 写色）共用一个真值源。
struct ColorToken {
  std::string_view name;
  math::Color Palette::*member;
};

constexpr ColorToken kColorTokens[] = {
    {"bg", &Palette::bg},
    {"surface", &Palette::surface},
    {"surface_alt", &Palette::surface_alt},
    {"surface_sunken", &Palette::surface_sunken},
    {"surface_raised", &Palette::surface_raised},
    {"border", &Palette::border},
    {"border_strong", &Palette::border_strong},
    {"border_subtle", &Palette::border_subtle},
    {"border_hover", &Palette::border_hover},
    {"text", &Palette::text},
    {"text_muted", &Palette::text_muted},
    {"text_faint", &Palette::text_faint},
    {"primary", &Palette::primary},
    {"primary_hover", &Palette::primary_hover},
    {"primary_active", &Palette::primary_active},
    {"primary_soft", &Palette::primary_soft},
    {"on_primary", &Palette::on_primary},
    {"accent", &Palette::accent},
    {"accent_soft", &Palette::accent_soft},
    {"success", &Palette::success},
    {"warning", &Palette::warning},
    {"danger", &Palette::danger},
    {"surface_hover", &Palette::surface_hover},
    {"surface_pressed", &Palette::surface_pressed},
    {"glow", &Palette::glow},
    {"shadow_glow", &Palette::shadow_glow},
    {"highlight", &Palette::highlight},
    {"focus_ring", &Palette::focus_ring},
    {"overlay", &Palette::overlay},
    {"shadow", &Palette::shadow},
    {"selection", &Palette::selection},
    {"code_bg", &Palette::code_bg},
    {"code_border", &Palette::code_border},
};

struct MetricToken {
  std::string_view name;
  float Metrics::*member;
  /// 写入时的合法区间（含）；`min > max` 表示不限。
  /// 为什么要夹取：主题文件/协议参数是**外部输入**，一个负的圆角或 0 字号会让界面消失，
  /// 而这属于"配置写错了不该让进程崩"的范畴（与 `json_get_*` 退化为默认值同姿态）。
  float minimum;
  float maximum;
};

constexpr MetricToken kMetricTokens[] = {
    {"space_xs", &Metrics::space_xs, 0.0f, 256.0f},
    {"space_sm", &Metrics::space_sm, 0.0f, 256.0f},
    {"space_md", &Metrics::space_md, 0.0f, 256.0f},
    {"space_lg", &Metrics::space_lg, 0.0f, 256.0f},
    {"space_xl", &Metrics::space_xl, 0.0f, 256.0f},
    {"space_2xl", &Metrics::space_2xl, 0.0f, 256.0f},
    {"radius_sm", &Metrics::radius_sm, 0.0f, 256.0f},
    {"radius_md", &Metrics::radius_md, 0.0f, 256.0f},
    {"radius_lg", &Metrics::radius_lg, 0.0f, 256.0f},
    {"radius_xl", &Metrics::radius_xl, 0.0f, 256.0f},
    {"radius_pill", &Metrics::radius_pill, 0.0f, 4096.0f},
    {"font_2xs", &Metrics::font_2xs, 4.0f, 256.0f},
    {"font_xs", &Metrics::font_xs, 4.0f, 256.0f},
    {"font_sm", &Metrics::font_sm, 4.0f, 256.0f},
    {"font_base", &Metrics::font_base, 4.0f, 256.0f},
    {"font_lg", &Metrics::font_lg, 4.0f, 256.0f},
    {"font_xl", &Metrics::font_xl, 4.0f, 256.0f},
    {"font_2xl", &Metrics::font_2xl, 4.0f, 256.0f},
    {"font_3xl", &Metrics::font_3xl, 4.0f, 256.0f},
    {"line_height_body", &Metrics::line_height_body, 1.0f, 4.0f},
    {"line_height_heading", &Metrics::line_height_heading, 1.0f, 4.0f},
    {"control_height_sm", &Metrics::control_height_sm, 8.0f, 256.0f},
    {"control_height", &Metrics::control_height, 8.0f, 256.0f},
    {"control_height_lg", &Metrics::control_height_lg, 8.0f, 256.0f},
    {"icon_size_sm", &Metrics::icon_size_sm, 4.0f, 256.0f},
    {"icon_size", &Metrics::icon_size, 4.0f, 256.0f},
    {"icon_size_lg", &Metrics::icon_size_lg, 4.0f, 256.0f},
    {"border_width", &Metrics::border_width, 0.0f, 16.0f},
    {"focus_width", &Metrics::focus_width, 0.0f, 16.0f},
    {"motion_fast", &Metrics::motion_fast, 0.0f, 10000.0f},
    {"motion_normal", &Metrics::motion_normal, 0.0f, 10000.0f},
    {"motion_slow", &Metrics::motion_slow, 0.0f, 10000.0f},
    // 缓动幂次：上限取 8（再高就近似“瞬间到位后再爬一段”，没有实际用途）。
    {"motion_ease_out_power", &Metrics::motion_ease_out_power, 0.0f, 8.0f},
    {"hover_lift", &Metrics::hover_lift, 0.0f, 64.0f},
    {"hover_glow_width", &Metrics::hover_glow_width, 0.0f, 64.0f},
    {"press_sink", &Metrics::press_sink, 0.0f, 64.0f},
    {"shadow_strength", &Metrics::shadow_strength, 0.0f, 4.0f},
    {"shadow_spread", &Metrics::shadow_spread, 0.1f, 4.0f},
    // 弹层毛玻璃（2026-10 新增）。必须进 token 表，否则主题文件/控制通道改不了它们——
    // 而"从 CSS 主题移植"恰恰最需要：歌白十个主题的 `--glass` 半径各不相同
    //（18~28px），tint 也必须按明暗分别给（亮色低了弹层就“陷进去”）。
    {"overlay_blur_radius", &Metrics::overlay_blur_radius, 0.0f, 128.0f},
    {"overlay_blur_tint", &Metrics::overlay_blur_tint, 0.0f, 1.0f},
    {"shadow_glow_strength", &Metrics::shadow_glow_strength, 0.0f, 4.0f},
};

// `hover_duration` 是 `double`（秒）——单独处理。
//
// 为什么不为它把整张表改成 `double`：其余 30 多个尺度令牌都是 `float`（布局量），
// 为了一个成员把全部类型放宽，反而掩盖了"尺度令牌该是 `float`"这个约束。
using DoubleMetric = double Metrics::*;
constexpr std::string_view kHoverDuration{"hover_duration"};
constexpr DoubleMetric kHoverDurationMember = &Metrics::hover_duration;

}  // namespace

auto palette_token_names() -> const std::vector<std::string_view>& {
  static const std::vector<std::string_view> names = [] {
    std::vector<std::string_view> out;
    out.reserve(std::size(kColorTokens));
    for (const ColorToken& token : kColorTokens) out.push_back(token.name);
    return out;
  }();
  return names;
}

auto palette_token(const Palette& palette, std::string_view name) -> std::optional<math::Color> {
  for (const ColorToken& token : kColorTokens) {
    if (token.name == name) return palette.*(token.member);
  }
  return std::nullopt;
}

auto set_palette_token(Palette& palette, std::string_view name, math::Color color) -> bool {
  for (const ColorToken& token : kColorTokens) {
    if (token.name == name) {
      palette.*(token.member) = color;
      return true;
    }
  }
  return false;
}

auto metric_token_names() -> const std::vector<std::string_view>& {
  static const std::vector<std::string_view> names = [] {
    std::vector<std::string_view> out;
    out.reserve(std::size(kMetricTokens) + 1);
    for (const MetricToken& token : kMetricTokens) out.push_back(token.name);
    out.push_back(kHoverDuration);
    return out;
  }();
  return names;
}

auto metric_token(const Metrics& metrics, std::string_view name) -> std::optional<float> {
  for (const MetricToken& token : kMetricTokens) {
    if (token.name == name) return metrics.*(token.member);
  }
  if (name == kHoverDuration) return static_cast<float>(metrics.*kHoverDurationMember);
  return std::nullopt;
}

auto set_metric_token(Metrics& metrics, std::string_view name, float value) -> bool {
  if (!(value == value)) return false;  // NaN：宁可拒绝也不要让它污染布局
  for (const MetricToken& token : kMetricTokens) {
    if (token.name == name) {
      metrics.*(token.member) = std::clamp(value, token.minimum, token.maximum);
      return true;
    }
  }
  if (name == kHoverDuration) {
    metrics.*kHoverDurationMember = static_cast<double>(std::clamp(value, 0.0f, 10.0f));
    return true;
  }
  return false;
}

}  // namespace st::ui
