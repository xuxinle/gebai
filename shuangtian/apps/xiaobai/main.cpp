/// 小白（xiaobai）——极简 AI 智能体。
///
/// ## 它是什么
///
/// 桌面上的一个极简 AI 对话窗口：只保留最关键的两件事——**对话**与**模型配置**。
/// 模型服务**仅支持 OpenAI `/v1/chat/completions` 规范**（任意 OpenAI 兼容端点）。
///
/// ## 形态
///
/// - 界面：C++ 声明式（`st::ui::dsl`），模式照 caiyun（WindowFrame 壳 + mount_into）。
/// - 传输：`st::net::connect_tcp` + `st::net::TlsStream`（Windows 走 Schannel，零依赖）。
///   不走 `HttpClient`：它是「整份读完才返回」的模型，拿不到流式增量。
/// - 流式：后台线程读 socket → 主循环每帧取增量 → 追加到助手气泡（打字机效果）。
///   后台线程**只写自己的 mutex 保护缓冲区**，绝不触碰 UI 状态（重组是单线程的）。
/// - 配置：JSON 持久化到 `~/.xiaobai/config.json`。
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "st/app/app.hpp"
#include "st/app/cli.hpp"
#include "st/core/entry.hpp"
#include "st/core/error.hpp"
#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/core/net.hpp"
#include "st/core/time.hpp"
#include "st/net/http.hpp"
#include "st/net/tls.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/text.hpp"
#include "st/ui/components/text_area.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/icon.hpp"

namespace {

using st::Json;
namespace http = st::net::http;

// 悬浮输入胶囊：最多长到 6 行（超过就内部滚动）。
// 尺寸、行高、圆角、高度全部由框架算：应用不再手算任何几何。
inline constexpr std::size_t kComposerMaxLines = 6;

/// 消息区尾部留白：胶囊是浮层（不占流），按最大高度预留才算得太少。
/// 该值只是“预留得够用”，不影响胶囊本身（那个自适应）。
inline constexpr float kComposerReserve = 160.0f;

/// 气泡最大宽度（逻辑像素）：气泡按**内容**自适应，只卡一个上限——
/// 写死宽度会让短句启大片空白、长句又被砍。窗口宽 720 时约 2/3 屏。
inline constexpr float kMaxBubbleWidth = 520.0f;

// 默认模型：应用内置的兜底值（不是密钥，也不是部署配置）。
// 真正的部署参数走环境变量（见 `env_or`）或 `~/.xiaobai/config.json`，
// **不要把端点/密钥写进代码**——代码只提供中性的默认值。
inline constexpr const char* kDefaultModel = "deepseek-v4.1-flash";
inline constexpr const char* kDefaultBaseUrl = "https://api.deepseek.com/v1";

/// 读环境变量（不存在或空串 → 返回 `fallback`）。
[[nodiscard]] auto env_or(const char* name, const std::string& fallback) -> std::string {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') return fallback;
  return std::string(raw);
}

// ———————————————————————————————————————————————————————————————
// 配置
//
// 三层来源（后者盖前者）：
//   ① 代码内置默认值（中性：公开的默认端点 + 默认模型名）；
//   ② 环境变量 `XIAOBAI_BASE_URL` / `XIAOBAI_API_KEY` / `XIAOBAI_MODEL`；
//   ③ `~/.xiaobai/config.json`（用户在设置窗里填的）。
// 为何要有②：部署/打包时注入配置不应该改代码或往仓库里塞密钥——
// 环境变量是标准做法，且本应用启动时就把它们吃进内存，不落盘。
// ———————————————————————————————————————————————————————————————
struct Config {
  std::string base_url{kDefaultBaseUrl};
  std::string api_key{};
  std::string model{kDefaultModel};

  [[nodiscard]] auto ready() const -> bool {
    return !base_url.empty() && !api_key.empty() && !model.empty();
  }
  [[nodiscard]] auto as_json() const -> Json {
    // 必须显式 Json::object()：Json{{{...}}} 会被初始化列表规则解成「数组里一个对」，
    // 实测写出过 `[{...}]` 而读回时对象字段全落空 → 表现为“保存了但重启后配置丢失”。
    Json out = Json::object();
    out["base_url"] = base_url;
    out["api_key"] = api_key;
    out["model"] = model;
    return out;
  }
  [[nodiscard]] static auto from_json(const Json& j) -> Config {
    Config c;
    // 容错：早先版本可能写出过单元素数组形态，遇到时取第一个元素。
    const Json* root = &j;
    if (j.is_array() && !j.empty()) root = &j[0];
    c.base_url = st::json_get_string(*root, "base_url", c.base_url);
    c.api_key = st::json_get_string(*root, "api_key", c.api_key);
    c.model = st::json_get_string(*root, "model", c.model);
    return c;
  }
  [[nodiscard]] auto operator==(const Config& other) const -> bool = default;
};

/// 模型名是否为“部署注入”（环境变量给了值）。
///
/// 用来决定设置窗要不要暴露模型字段：部署方已经定死时，让用户在界面上改它只会
/// 造成“改完不生效”（下一个启动又回到环境变量）——不如不展示。
[[nodiscard]] auto model_injected_by_env() -> bool {
  const char* raw = std::getenv("XIAOBAI_MODEL");
  return raw != nullptr && *raw != '\0';
}

[[nodiscard]] auto config_dir() -> std::string {
  const std::string home = st::fs::home_dir();
  return st::fs::join(home.empty() ? std::string(".") : home, ".xiaobai");
}
[[nodiscard]] auto config_path() -> std::string { return st::fs::join(config_dir(), "config.json"); }

/// 加载配置：内置默认 → 环境变量 → 配置文件（后者盖前者）。
///
/// 环境变量优先于配置文件，是为了让“打包/部署注入”能压过用户本地存的值——
/// 否则用户机器上旧的一份配置会让分发出去的新包看着没生效。
[[nodiscard]] auto load_config() -> Config {
  Config c;
  c.base_url = env_or("XIAOBAI_BASE_URL", c.base_url);
  c.api_key = env_or("XIAOBAI_API_KEY", c.api_key);
  c.model = env_or("XIAOBAI_MODEL", c.model);
  if (const auto parsed = st::json_parse_file(config_path()); parsed.has_value()) {
    Config file_cfg = Config::from_json(*parsed);
    // 环境变量给了值的项不被文件覆盖（上面那两句已经把 env 盖到 c 上了）。
    if (env_or("XIAOBAI_BASE_URL", "").empty()) c.base_url = file_cfg.base_url;
    if (env_or("XIAOBAI_API_KEY", "").empty()) c.api_key = file_cfg.api_key;
    if (env_or("XIAOBAI_MODEL", "").empty()) c.model = file_cfg.model;
  }
  return c;
}

void save_config(const Config& c) {
  (void)st::fs::create_directories(config_dir());
  if (auto status = st::json_write_file(config_path(), c.as_json()); !status) {
    st::log::out("[xiaobai] 配置写盘失败（{}）：{}\n", config_path(), status.error().message);
  }
}

// ———————————————————————————————————————————————————————————————
// 消息
// ———————————————————————————————————————————————————————————————
struct Message {
  std::string role;  ///< "user" | "assistant"
  std::string content;
  std::string stamp;  ///< HH:MM（展示用）

  [[nodiscard]] auto operator==(const Message& other) const -> bool = default;
};

// ———————————————————————————————————————————————————————————————
// 后台流式任务
//
// 只做两件事：把收到的增量文本塞进 `buffer`（互斥保护），把结束/错误标记置位。
// UI 状态一律由主线程（build 里）从 buffer 取走后写——跨线程写 State 会与
// 遍历中的元素树竞争（框架硬约束）。
// ———————————————————————————————————————————————————————————————
struct StreamJob {
  std::mutex mutex{};
  std::string buffer{};
  std::string error{};
  std::atomic<bool> finished{false};
  std::atomic<bool> cancelled{false};
  std::thread worker{};

  void push(std::string piece) {
    const std::lock_guard<std::mutex> guard(mutex);
    buffer += piece;
  }
  void fail(std::string message) {
    const std::lock_guard<std::mutex> guard(mutex);
    if (error.empty()) error = std::move(message);
  }
  /// 取走已收增量（错误也一并通过 `error` 字段返回）。
  [[nodiscard]] auto drain(std::string& out_error) -> std::string {
    const std::lock_guard<std::mutex> guard(mutex);
    std::string out = std::move(buffer);
    buffer.clear();
    out_error = error;
    return out;
  }
};

// ———————————————————————————————————————————————————————————————
// 流式读取（TCP / TLS 两形态共用；两者接口在「读写/等待」这三点上一致，
// 但类型无关，所以用模板。注意 `wait_readable` 只有 TcpStream 有 ——
// TLS 侧退化为阻塞读（TlsStream 没有暴露等待原语）。
// ———————————————————————————————————————————————————————————————
template <class Stream>
void pump_stream(Stream& stream, StreamJob& job, const std::string& wire) {
  if (auto status = stream.write_all(std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()));
      !status) {
    job.fail("发送请求失败：" + status.error().message);
    return;
  }
  std::vector<std::uint8_t> buf(32 * 1024);
  std::string pending;
  while (!job.cancelled.load()) {
    auto read = stream.read_some(std::span<std::uint8_t>(buf.data(), buf.size()));
    if (!read.has_value()) {
      job.fail("读取失败：" + read.error().message);
      return;
    }
    if (*read == 0) break;  // 对端关闭 = 正常结束
    pending.append(reinterpret_cast<const char*>(buf.data()), *read);
    // 交付使用方（SSE 解析在外部按行处理，这里只负责把字节流推出去）
    job.push(std::move(pending));
    pending.clear();
  }
}

/// 把 URL 与请求体拼成 OpenAI 兼容的完整报文。
[[nodiscard]] auto build_request(const Config& cfg, const std::vector<Json>& messages,
                                 std::string& out_url) -> std::string {
  std::string url = cfg.base_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  // 端点已含路径就不再拼（允许用户直接填完整的 .../chat/completions）
  if (url.find("/chat/") == std::string::npos) url += "/chat/completions";
  out_url = url;

  Json payload = Json::object();
  payload["model"] = cfg.model;
  payload["messages"] = messages;
  payload["stream"] = true;
  const std::string body = st::json_dump(payload, false);

  http::HttpRequest req;
  req.method = http::Method::Post;
  const auto parsed = http::Url::parse(url);
  req.path = (parsed.has_value() && !parsed->path.empty()) ? parsed->path : "/v1/chat/completions";
  req.host = parsed.has_value() ? parsed->host : std::string{};
  req.port = parsed.has_value() ? parsed->port : 80;
  req.headers.set("Content-Type", "application/json");
  req.headers.set("Authorization", "Bearer " + cfg.api_key);
  req.headers.set("Accept", "text/event-stream");
  req.headers.set("Connection", "close");
  req.body = body;

  std::string wire = http::serialize_request(req);
  // Host 头必须带端口（非默认端口时），serialize_request 已按 req.host/port 处理。
  return wire;
}

/// 从 SSE 行里取出增量文本；`[DONE]` 返回 nullopt 并置结束标记。
[[nodiscard]] auto parse_sse_line(std::string_view line) -> std::optional<std::string> {
  if (!line.starts_with("data:")) return std::nullopt;
  std::string_view data = line.substr(5);
  while (!data.empty() && (data.front() == ' ' || data.front() == '\t')) data.remove_prefix(1);
  if (data.empty() || data == "[DONE]") return std::nullopt;
  const auto parsed = st::json_parse(data);
  if (!parsed.has_value()) return std::nullopt;
  const Json* choices = st::json_find(*parsed, "choices");
  if (choices == nullptr || !choices->is_array() || choices->empty()) return std::nullopt;
  const Json* delta = st::json_find((*choices)[0], "delta");
  if (delta == nullptr) return std::nullopt;
  const Json* content = st::json_find(*delta, "content");
  if (content == nullptr || !content->is_string()) return std::nullopt;
  return st::json_as_string(*content);
}

// ———————————————————————————————————————————————————————————————
// 主页面
// ———————————————————————————————————————————————————————————————
struct AgentPage : st::ui::dsl::Component {
  using Component::Component;

  // —— 响应式状态 ——
  st::ui::dsl::State<std::vector<Message>> messages{std::vector<Message>{}};
  st::ui::dsl::State<std::string> draft{""};
  st::ui::dsl::State<bool> busy{false};
  st::ui::dsl::State<std::string> status{"就绪"};
  st::ui::dsl::State<bool> settings_open{false};
  st::ui::dsl::State<Config> config{load_config()};
  // 设置弹窗的编辑缓冲（打开时从 config 拷贝，保存时写回）
  st::ui::dsl::State<std::string> draft_url{};
  st::ui::dsl::State<std::string> draft_key{};
  st::ui::dsl::State<std::string> draft_model{};

  /// 当前请求（非空 = 正在流式生成）。
  std::unique_ptr<StreamJob> job{};
  /// 流式输出累积的正文（主线程侧，属于当前助手消息）。
  std::string stream_text{};

  AgentPage() {
    // 首条欢迎消息
    messages.set(std::vector<Message>{
        Message{"assistant", "你好，我是小白。\n\n先在「设置」里填好 OpenAI 兼容端点与 API Key，然后在下方输入问题。", ""}});
  }

  // —— 声明式构建 ——
  void build(st::ui::dsl::Composer& c) override {
    using namespace st::ui::dsl;

    (void)column(c, {.gap = 0, .grow = true, .key = "root"}, [&] {
      // ── 顶栏 ──
      (void)row(c, {.gap = 8, .padding_x = 12, .padding_y = 8,
                    .surface = st::ui::Element::Surface::Alt, .key = "topbar"}, [&] {
        (void)icon(c, "sparkles", 18, {.color = st::ui::Tone::Accent, .key = "brand-icon"});
        (void)text(c, [] { return std::string("小白"); },
                   {.size = 13.0f, .weight = st::ui::FontWeight::Bold,
                    .id = "app-name", .key = "app-name"});
        (void)text(c, [&] {
                     const auto model = config.value().model;
                     return model.empty() ? std::string("（未绑定模型）") : ("· " + model);
                   },
                   {.color = st::ui::Tone::Muted, .size = 12.0f,
                    .id = "model-label", .key = "model-label"});
        (void)spacer(c);
      });
      (void)divider(c, false);

      // ── 消息区 ──
      //
      // 底部留白 = 胶囊高度 + 两侧边距，否则最后一条消息会被悬浮胶囊压住。
      auto& scroll = custom<st::ui::ScrollView>(
          c,
          [](st::ui::ScrollView& s) {
            s.set_id("msg-scroll");
            s.set_show_scrollbar(true);
          },
          {.grow = true, .key = "msg-scroll"});
      {
        BuildScope scope(c, &scroll);
        (void)column(c, {.gap = 10, .padding_x = 14, .padding_y = 14,
                         .key = "msg-list"}, [&] {
          for (const auto& m : messages.value()) bubble(c, m);
          // 生成/错误状态显示在消息流**末尾**（原顶栏那条状态行已去掉，
          // 因为常驻一行状态看着像多余的仪表盘；而这两类信息与对话上下文相关，
          // 放在气泡后面更自然）。
          if (!status.value().empty() && status.value() != "就绪") {
            (void)text(c, [&] { return status.value(); },
                       {.color = st::ui::Tone::Muted, .size = 12.0f,
                        .id = "status-line", .key = "status-line"});
          }
          // 尾巴留白：胶囊是浮层（不占流），按**最大高度**预留，
          // 否则内容变多后最后几条会被胶囊压住。
          (void)spacer(c, kComposerReserve);
        });
      }

      // ── 输入区：胶囊悬浮式（浮在消息区之上，不占一条通栏）──
      // 歌白同款：大圆角胶囊 + 浮层阴影，发送钮嵌在胶囊内右端。
      //
      // 高度与圆角都不需要应用手算：
      // - 输入框 `set_auto_grow(true)` 自己跟随内容长高（1~6 行）；
      // - 胶囊不写 height，只锚 `bottom`（单边锚定 = 贴底、尺寸跟内容）；
      // - 圆角给一个极大值（999）：绘制层按 CSS `border-radius` 语义**夹取到短边一半**，
      //   于是矮时是胶囊、长高后自动变圆角矩形（已沉到 `Canvas::fill_rect` 统一处理）。
      (void)custom_container<st::ui::Panel>(
          c,
          [&] {
            (void)custom<st::ui::TextArea>(
                c,
                [this](st::ui::TextArea& ta) {
                  ta.set_id("msg-input");
                  ta.set_placeholder("发消息给小白…");
                  // 外框完全交给胶囊：底色、描边、焦点环都不画。
                  ta.set_seamless_background(true);
                  ta.set_show_border(false);
                  // 高度跟随内容，最多 6 行（超出内部滚动）。
                  ta.set_auto_grow(true);
                  ta.set_lines_range(1, kComposerMaxLines);
                  if (ta.value() != draft.value()) ta.update_text(draft.value());
                  ta.on_change = [this](std::string_view v) { draft.set(std::string(v)); };
                },
                {.grow = true, .key = "msg-input"});
            (void)button(c, busy.value() ? "停止" : "发送",
                         [this] { busy.value() ? stop_stream() : send(); },
                         {.variant = busy.value() ? st::ui::Variant::Danger
                                                  : st::ui::Variant::Primary,
                          .radius_mode = st::ui::RadiusMode::Pill,
                          .id = "btn-send", .key = "btn-send"});
          },
          [](st::ui::Panel&) {},   // 无样式逃生：胶囊的每一面都走 BoxProps 声明
          {.gap = 8, .padding_x = 8, .padding_y = 6,
           .direction = st::ui::FlexDirection::Row,
           .radius = 999.0f,        // 胶囊：绘制层夹到短边一半（CSS 语义）
           .surface = st::ui::Element::Surface::Base,
           .absolute = true,
           .left = 18.0f,
           .right = 18.0f,
           .bottom = 18.0f,
           .radius_max = 22.0f,     // 矮时胶囊，长高后圆角矩形
           .border_width = 1.0f,
           .border_color = st::math::Color{0, 0, 0, 36},
           .elevation = st::ui::Elevation::Overlay,
           .align_items = st::ui::Align::Center,
           .key = "composer"});

      // ── 配置面板（绝对定位盖在内容区之上，但**不越过标题栏**）──
      //
      // 为何不用 `overlay()`：overlay 挂在 UiRoot 顶层，会连 WindowFrame 的
      // 标题栏（拖动区/最小化/关闭按钮）一起盖住——实测现象是“弹窗出来了但窗口拖不动”。
      // 声明在本页 column 里 + `.absolute` 就只盖内容区；`intercepts` 默认 true，
      // 于是下层输入区/发送钮在弹窗期间收不到点击。
      if (settings_open.value()) {
        // 全页遮罩：绝对定位四边归零（两端都锚定 = 铺满）+ 半透明填充。
        // `intercepts` 默认 true，于是下层输入区/发送钮在弹窗期间收不到点击。
        (void)column(c, {.padding = 24,
                         .surface = st::ui::Element::Surface::None,
                         .fill = st::math::Color{0, 0, 0, 56},
                         .absolute = true,
                         .left = 0.0f,
                         .top = 0.0f,
                         .right = 0.0f,
                         .bottom = 0.0f,
                         .justify = st::ui::Justify::Center,
                         .align_items = st::ui::Align::Center,
                         .key = "settings-mask"},
                     [&] { settings_panel(c); });
      }
    });
  }

  /// 一条消息气泡：用户靠右、助手靠左；正文用 MarkdownView 渲染。
  ///
  /// 宽度**按内容自适应**（短句就是小气泡），只设一个上限（`kMaxBubbleWidth`）：
  /// 写死像素会让短句留大片空白、长句又被封顶后浪费空间。
  void bubble(st::ui::dsl::Composer& c, const Message& m) {
    using namespace st::ui::dsl;
    const bool is_user = m.role == "user";
    (void)row(c, {.gap = 0, .key = "row-" + m.role + "-" + std::to_string(messages.value().size())},
              [&] {
                if (is_user) (void)spacer(c);
                (void)card(c, {.gap = 4, .padding = 10, .max_width = kMaxBubbleWidth,
                               .surface = is_user ? st::ui::Element::Surface::Sunken
                                                  : st::ui::Element::Surface::Base,
                               .key = "bubble-" + m.role},
                           [&] {
                             (void)row(c, {.gap = 6, .key = "meta"}, [&] {
                               (void)icon(c, is_user ? "user" : "sparkles", 12,
                                          {.color = is_user ? st::ui::Tone::Muted
                                                            : st::ui::Tone::Accent,
                                           .key = "meta-icon"});
                               (void)text(c, [&] { return is_user ? std::string("你") : std::string("AI"); },
                                          {.size = 11.0f, .weight = st::ui::FontWeight::Bold,
                                           .key = "meta-name"});
                               if (!m.stamp.empty()) {
                                 (void)text(c, [&]() -> std::string { return m.stamp; },
                                            {.color = st::ui::Tone::Muted, .size = 10.0f,
                                             .key = "meta-time"});
                               }
                             });
                             // 用 `custom<MarkdownView>` 而不 `markdown()`：需要
                             // “按内容收缩宽度”（气泡随短消息变窄）。
                             // 代价：`markdown()` 的惰性 source 闭包（自动订阅）要手动补——
                             // 先在该帧读一次 State 登记依赖，再把值写进去。
                             const std::string body = m.content;   // 读值 = 登记依赖
                             (void)custom<st::ui::MarkdownView>(
                                 c,
                                 [body](st::ui::MarkdownView& view) {
                                   view.set_shrink_to_fit(true);
                                   view.set_base_font_size(13.0f);
                                   // `set_markdown` 自带相等早退，逐帧重设不贵。
                                   view.set_markdown(body);
                                 },
                                 {.key = "body-" + m.role});
                           });
                if (!is_user) (void)spacer(c);
              });
  }

  /// 配置面板（overlay 内容）。
  ///
  /// 无顶栏入口：未配置时**启动即弹**（首屏必须完成）；已配置则不弹。
  /// 因此这里不给「取消」——缺配置的状态下取消只会得到一个不能用的窗口。
  void settings_panel(st::ui::dsl::Composer& c) {
    using namespace st::ui::dsl;
    // 定位与居中由 build() 里的 `.absolute` 容器负责；这里只声明卡片本体。
    (void)card(c, {.gap = 12, .padding = 18, .width = 480.0f,
                   .surface = st::ui::Element::Surface::Base,
                   .elevation = st::ui::Elevation::Raised,
                   .key = "settings-card"},
               [&] {
                 (void)heading(c, "连接配置", 3);
                 (void)text(c, [] { return std::string("仅支持 OpenAI /v1/chat/completions 规范"); },
                            {.color = st::ui::Tone::Muted, .size = 12.0f, .key = "cfg-hint"});

                 settings_field(c, "Base URL", "（如 https://api.openai.com/v1）",
                                draft_url.value(),
                                [this](std::string v) { draft_url.set(std::move(v)); },
                                "cfg-url");
                 settings_field(c, "API Key", "（Bearer 令牌）", draft_key.value(),
                                [this](std::string v) { draft_key.set(std::move(v)); },
                                "cfg-key", /*password=*/true);
                 // 模型名：部署方用 `XIAOBAI_MODEL` 环境变量定死时不展示
                 // （展示也改不动——下次启动又回到环境变量的值）。
                 if (!model_injected_by_env()) {
                   settings_field(c, "模型", "（模型名，如 deepseek-v4.1-flash）",
                                  draft_model.value(),
                                  [this](std::string v) { draft_model.set(std::move(v)); },
                                  "cfg-model");
                 }

                 (void)row(c, {.gap = 8, .justify = st::ui::Justify::End,
                               .key = "cfg-actions"}, [&] {
                   (void)button(c, "开始使用", [this] { save_settings(); },
                                {.variant = st::ui::Variant::Primary,
                                 .id = "cfg-save", .key = "cfg-save"});
                 });
               });
  }

  void settings_field(st::ui::dsl::Composer& c, const std::string& label, const std::string& hint,
                      const std::string& value, std::function<void(std::string)> on_input,
                      const std::string& field_id, bool password = false) {
    using namespace st::ui::dsl;
    (void)column(c, {.gap = 4, .key = field_id + "-wrap"}, [&] {
      (void)text(c, [label] { return label; }, {.size = 12.0f, .weight = st::ui::FontWeight::Bold,
                                                .key = field_id + "-label"});
      (void)input(c, value, std::move(on_input),
                  {.id = field_id, .key = field_id}, password);
      (void)text(c, [hint] { return hint; }, {.color = st::ui::Tone::Muted, .size = 11.0f,
                                              .key = field_id + "-hint"});
    });
  }

  // —— 行为 ——
  void open_settings() {
    const Config cfg = config.value();
    draft_url.set(cfg.base_url);
    draft_key.set(cfg.api_key);
    draft_model.set(cfg.model);
    settings_open.set(true);
  }

  void save_settings() {
    Config cfg = config.value();
    cfg.base_url = draft_url.value();
    cfg.api_key = draft_key.value();
    if (!model_injected_by_env()) cfg.model = draft_model.value();  // 环境变量注入的模型不被界面覆盖
    config.set(cfg);
    save_config(cfg);
    settings_open.set(false);
    status.set(cfg.ready() ? std::string{} : "配置不完整（需要 base_url / api_key）");
  }

  void send() {
    const std::string text = trim(draft.value());
    if (text.empty() || busy.value()) return;
    const Config cfg = config.value();
    if (!cfg.ready()) {
      status.set("请先完成连接配置");
      if (!settings_open.value()) open_settings();
      return;
    }

    append_message(Message{"user", text, now_stamp()});
    append_message(Message{"assistant", "", now_stamp()});
    draft.set({});
    stream_text.clear();
    status.set("正在连接…");
    busy.set(true);

    // 上下文 = 全部历史（去掉最后那条空的助手占位）
    std::vector<Json> context;
    for (const auto& m : messages.value()) {
      if (m.content.empty()) continue;
      Json entry = Json::object();
    entry["role"] = m.role;
    entry["content"] = m.content;
    context.push_back(std::move(entry));
    }

    auto next = std::make_unique<StreamJob>();
    StreamJob* raw = next.get();
    Config cfg_copy = cfg;
    raw->worker = std::thread([this, raw, cfg_copy, context = std::move(context)] {
      run_request(raw, cfg_copy, context);
    });
    job = std::move(next);
  }

  void stop_stream() {
    if (job == nullptr) return;
    job->cancelled.store(true);
    status.set("正在停止…");
  }

  /// 主循环侧：把后台收到的增量并入当前助手消息。
  /// 返回本次是否改动了状态（改动则需要 request_repaint）。
  auto pump_stream_into_state() -> bool {
    if (job == nullptr) return false;
    bool touched = false;

    std::string error;
    std::string chunk = job->drain(error);
    if (!chunk.empty()) {
      stream_text += chunk;
      auto list = messages.value();
      if (auto updated = last_assistant_index(&list); updated.has_value()) {
        list[*updated].content = stream_text;
        messages.set(std::move(list));
      }
      status.set("生成中…（" + std::to_string(stream_text.size()) + " 字符）");
      touched = true;
    }

    if (job->finished.load()) {
      if (!error.empty() && error != "cancelled") {
        auto list = messages.value();
        if (auto updated = last_assistant_index(&list); updated.has_value()) {
          list[*updated].content = stream_text.empty() ? "**请求失败**：" + error : stream_text;
          messages.set(std::move(list));
        }
        status.set("请求失败：" + error);
      } else if (error == "cancelled") {
        status.set("已停止");
      } else {
        status.set({});   // 生成结束即清空：常态不占状态文字
      }
      if (job->worker.joinable()) job->worker.join();
      job.reset();
      stream_text.clear();
      busy.set(false);
      touched = true;
    }
    return touched;
  }

  [[nodiscard]] static auto last_assistant_index(const std::vector<Message>* list)
      -> std::optional<std::size_t> {
    if (list == nullptr) return std::nullopt;
    for (std::size_t i = list->size(); i > 0; --i) {
      if ((*list)[i - 1].role == "assistant") return i - 1;
    }
    return std::nullopt;
  }

  void append_message(Message m) {
    auto list = messages.value();
    list.push_back(std::move(m));
    messages.set(std::move(list));
  }

  // —— 后台线程主体 ——
  static void run_request(StreamJob* raw, const Config& cfg, const std::vector<Json>& context) {
    std::string url;
    const std::string wire = build_request(cfg, context, url);
    const auto parsed = http::Url::parse(url);
    if (!parsed.has_value()) {
      raw->fail("Base URL 非法：" + url);
      raw->finished.store(true);
      return;
    }
    const bool use_tls = parsed->scheme == "https";

    if (use_tls) {
      auto tls = st::net::TlsStream::connect(parsed->host, parsed->port, 15000);
      if (!tls.has_value()) {
        raw->fail("TLS 连接失败：" + tls.error().message);
        raw->finished.store(true);
        return;
      }
      pump_tls(*tls, *raw, wire);
    } else {
      auto tcp = st::net::connect_tcp(parsed->host, parsed->port, 15000);
      if (!tcp.has_value()) {
        raw->fail("TCP 连接失败：" + tcp.error().message);
        raw->finished.store(true);
        return;
      }
      pump_tcp(*tcp, *raw, wire);
    }
    raw->finished.store(true);
  }

  static void pump_tcp(st::net::TcpStream& stream, StreamJob& raw, const std::string& wire) {
    if (auto status = stream.write_all(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()));
        !status) {
      raw.fail("发送请求失败：" + status.error().message);
      return;
    }
    std::vector<std::uint8_t> buf(16 * 1024);
    std::string pending;
    while (!raw.cancelled.load()) {
      if (auto ready = stream.wait_readable(200); !ready.has_value() || !*ready) continue;
      auto read = stream.read_some(std::span<std::uint8_t>(buf.data(), buf.size()));
      if (!read.has_value()) {
        raw.fail("读取失败：" + read.error().message);
        return;
      }
      if (*read == 0) break;
      pending.append(reinterpret_cast<const char*>(buf.data()), *read);
      consume_sse_lines(pending, raw);
    }
    if (raw.cancelled.load()) {
      const std::lock_guard<std::mutex> guard(raw.mutex);
      if (raw.error.empty()) raw.error = "cancelled";
    }
  }

  static void pump_tls(st::net::TlsStream& stream, StreamJob& raw, const std::string& wire) {
    if (auto status = stream.write_all(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()));
        !status) {
      raw.fail("发送请求失败：" + status.error().message);
      return;
    }
    std::vector<std::uint8_t> buf(16 * 1024);
    std::string pending;
    while (!raw.cancelled.load()) {
      auto read = stream.read_some(std::span<std::uint8_t>(buf.data(), buf.size()));
      if (!read.has_value()) {
        raw.fail("读取失败：" + read.error().message);
        return;
      }
      if (*read == 0) break;
      pending.append(reinterpret_cast<const char*>(buf.data()), *read);
      consume_sse_lines(pending, raw);
    }
    if (raw.cancelled.load()) {
      const std::lock_guard<std::mutex> guard(raw.mutex);
      if (raw.error.empty()) raw.error = "cancelled";
    }
  }

  /// 从 `pending` 中切出完整行，解析 SSE 增量并推给 job；剩余半行留在 `pending`。
  static void consume_sse_lines(std::string& pending, StreamJob& raw) {
    std::size_t start = 0;
    while (true) {
      const std::size_t newline = pending.find('\n', start);
      if (newline == std::string::npos) break;
      std::string_view line(pending.data() + start, newline - start);
      start = newline + 1;
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      // HTTP 头与空行：空行表示头部结束，之后才是事件体；两者都直接跳过。
      if (line.empty() || line.find(':') == std::string_view::npos) continue;
      if (line.starts_with("data:")) {
        if (line.find("[DONE]") != std::string_view::npos) {
          raw.finished.store(true);
          break;
        }
        if (auto piece = parse_sse_line(line); piece.has_value()) raw.push(std::move(*piece));
      }
    }
    pending.erase(0, start);
  }

  // —— 小工具 ——
  [[nodiscard]] static auto trim(std::string text) -> std::string {
    const auto not_space = [](unsigned char ch) { return ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t'; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
  }

  [[nodiscard]] static auto now_stamp() -> std::string {
    const std::int64_t seconds = st::time::now_ms() / 1000;
    return std::format("{:02}:{:02}", (seconds / 3600) % 24, (seconds / 60) % 60);
  }
};

// ———————————————————————————————————————————————————————————————
// 入口
// ———————————————————————————————————————————————————————————————
int run_app(int argc, char** argv) {
  st::app::CommonOptions common;
  const std::vector<st::app::AppOption> app_options = {};
  st::app::AppOptionValues values;
  auto parsed = st::app::parse_options(argc, argv, app_options, common, values);
  if (!parsed) {
    std::fprintf(stderr, "参数解析失败：%s\n", parsed.error().message.c_str());
    return 1;
  }
  if (common.show_help) {
    std::printf("小白（xiaobai）——极简 AI 智能体\n用法：xiaobai [通用选项]\n配置写入：%s\n",
                config_path().c_str());
    return 0;
  }

  st::app::AppOptions options = common.app;
  if (options.width <= 0) options.width = 720;
  if (options.height <= 0) options.height = 560;
  if (options.title.empty()) options.title = "小白 · 霜天";

  st::app::Application app("xiaobai", "0.1.0", options);

  auto page = std::make_shared<AgentPage>();
  auto frame = std::make_unique<st::ui::WindowFrame>("小白");
  frame->set_id("window-frame");
  frame->set_window_control(&app);
  if (frame->title_bar() != nullptr) frame->title_bar()->set_id("titlebar");
  st::ui::Element* slot = frame->content();
  app.set_content(std::move(frame));

  auto host = st::ui::dsl::mount_into(app.root(), *slot, page);
  if (host == nullptr) {
    std::fprintf(stderr, "声明式挂载失败\n");
    return 1;
  }
  (void)host->tick();

  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败：%s\n", status.error().message.c_str());
    return 1;
  }

  // 首次运行引导：没有可用配置就把配置面板直接弹出来（无顶栏入口，这一步不可绕过）。
  // 必须在 start() 之后：弹窗走声明式 overlay，需要 UiRoot 与脚本宿主就绪。
  const Config cfg = page->config.value();
  if (cfg.ready()) {
    page->status.set({});   // 就绪是常态：不显示状态行
  } else {
    page->status.set("请先完成连接配置");
    page->open_settings();
  }

  while (!app.quit_requested()) {
    // 流式增量必须先于重组推进：State 在此标脏，紧接着 tick 才能把改动落地。
    // 为何不放在 build() 里：build 只在重组时跑，而重组又靠“状态脏了”触发——
    // 谁先动的问题会变成静默死锁（实测：状态永远停在「正在连接…」）。
    if (page->pump_stream_into_state()) app.request_repaint();
    if (app.root().tick_declarative_hosts() > 0U) app.request_repaint();
    app.tick();
  }
  // 收尾：结束仍在跑的后台线程
  if (page->job != nullptr) {
    page->job->cancelled.store(true);
    if (page->job->worker.joinable()) page->job->worker.join();
  }
  return 0;
}

}  // namespace

ST_MAIN(run_app)