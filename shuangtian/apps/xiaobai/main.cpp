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
#include <filesystem>
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

#include "../app_icon.hpp"
#include "st/app/app.hpp"
#include "st/app/cli.hpp"
#include "st/core/entry.hpp"
#include "st/core/error.hpp"
#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/core/net.hpp"
#include "st/core/process.hpp"
#include "st/core/time.hpp"
#include "st/net/http.hpp"
#include "st/net/tls.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/select_box.hpp"
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

/// 气泡最大宽度：**不设固定上限**（取 `kUnbounded`），让它能长到消息区的可用宽
/// （再往外就是窗口边距，由外层容器决定）。
///
/// 为何不限定一个固定像素（如 520）：那会把“一行就放得下的话”在 520 处硬折行，
/// 即使窗口中还有大片空白——用户看到的是一只被挤窄的气泡，而不是“随内容宽”。
/// 气泡的宽度语义是：**短内容随内容变窄，长内容铺满可用宽**；
/// 后一半只需交给父级约束，不需要额外的魔数。
inline constexpr float kMaxBubbleWidth = st::ui::kUnbounded;

// ── 气泡的液态玻璃配置 ──
//
// **一条字符串说完**（语法与 XML 的 `glass="…"`、控制通道的 `set glass=…`
// 完全相同——三处共用一个解析器，不会分叉）。
//
// 五个参数各管一件事，调参时必须分开看（混着调会在“亮 ↔ 透”之间来回摆动）：
//
// | 参数 | 管的那个量 | 调高的代价 |
// |---|---|---|
// | `blur` | 背板层次保留（“透不透”） | 背景细节被抹平 → 一块均匀色 |
// | `tint` | 亮度 / 白膜厚度 | 盖住背板 → “亮了但不透” |
// | `refraction` | 边缘透镜畸变（“液态感”） | 边缘被扯得厉害 |
// | `dispersion` | 边缘色散（彩虹纹） | 彩边明显 → 廉价 |
// | `highlight` | 内边缘高光（“硬度”），0~1 | 边缘过曝 |
//
// **`tint` 是“亮与透”的直接权衡，不可能两全**：它乘在实白 alpha 上——
// 高了亮但盖住背板（像不透明卡片），低了透但暗部透出来。所以“亮”不靠它，
// 而靠 `highlight`（只贴边提亮）与 `refraction`（只弯边缘）。
//
// 各档取值的实测依据（扫描台：多组并排 + 单变量扫描，见霜天子Agent 提示词 §5.5）：
// · `blur`：4 细条锐利但像“半透明”而非玻璃；8 柔和有层次；14 起糊成色块 → 取 6
// · `tint`：0.15 很透但正文对比不足；0.34 偏白 → 取 0.22
// · `refraction`：**已关**（取 0）——它对 14px 小圆角没有正面价值，只会在边缘把
//   背后背景采样进来，形成一条与卡片本体色相不一致的“杂色带”。
//   实测（控制变量，逐个开关）：关掉它后卡片顶缘 R−B 从 **−8~−14** 回到 **−5**
//   （与背景的 −5 一致），杂色消失；而关掉 `highlight` 时杂色**仍在**。
//   设成 20 时能看出“液态感”，但那是在大面积样张上；真实卡片上只会脏边。
inline constexpr std::string_view kGlassSpec =
    "blur:6,tint:0.22,refraction:0,dispersion:0,highlight:0.35";
/// 气泡圆角：折射的带宽≈圆角半径，直角会把液态效果藏掉。
inline constexpr float kGlassRadius = 14.0f;

// 默认模型：应用内置的兜底值（不是密钥，也不是部署配置）。
//
// 真正的部署参数有两条路，**都不依赖运行时环境变量**：
//   ① **构建期宏**（推荐）：`st build --define 'XIAOBAI_*="…"'`，由下面的
//      `XIAOBAI_PRESET_*` 条件编译进二进制——产物开箱即用，不用在目标机器
//      配任何环境变量，也不会把密钥留在磁盘上。
//   ② `~/.xiaobai/config.json`（用户在设置窗里填）。
// 环境变量仍支持（优先级最高）——便于临时切换/调试。
inline constexpr const char* kDefaultModel = "deepseek-v4.1-flash";
inline constexpr const char* kDefaultBaseUrl = "https://api.deepseek.com/v1";

// ———————————————————————————————————————————————————————————————
// 构建期预置（条件编译）
//
// 三者各自独立：给了哪个宏就预置哪个，没给的那项退回默认/配置文件。
// 判“宏是否给出”只用 `#if defined`——`sizeof` 在预处理指令里不可用。
// 空串宏（`XIAOBAI_MODEL=""`）在 `#if` 里与未定义等价地有用：
// 运行时用 `preset_string()` 再判一次长度，空串就退回默认值。
// ———————————————————————————————————————————————————————————————
#if defined(XIAOBAI_MODEL)
#define XIAOBAI_HAS_PRESET_MODEL 1
#define XIAOBAI_PRESET_MODEL XIAOBAI_MODEL
#else
#define XIAOBAI_PRESET_MODEL kDefaultModel
#endif

#if defined(XIAOBAI_BASE_URL)
#define XIAOBAI_HAS_PRESET_BASE_URL 1
#define XIAOBAI_PRESET_BASE_URL XIAOBAI_BASE_URL
#else
#define XIAOBAI_PRESET_BASE_URL kDefaultBaseUrl
#endif

#if defined(XIAOBAI_API_KEY)
#define XIAOBAI_HAS_PRESET_API_KEY 1
#define XIAOBAI_PRESET_API_KEY XIAOBAI_API_KEY
#else
#define XIAOBAI_PRESET_API_KEY ""
#endif

/// 预置字符串非空才算“预置了”（空串 = 没预置）。
[[nodiscard]] constexpr auto preset_nonempty(const char* text) noexcept -> bool {
  return text != nullptr && *text != '\0';
}

/// 模型名是否在**构建期**定死（决定设置窗要不要暴露模型字段）。
[[nodiscard]] constexpr auto model_preset_at_build() noexcept -> bool {
#if defined(XIAOBAI_HAS_PRESET_MODEL)
  return preset_nonempty(XIAOBAI_MODEL);
#else
  return false;
#endif
}

/// 端点是否在构建期定死。
[[nodiscard]] constexpr auto base_url_preset_at_build() noexcept -> bool {
#if defined(XIAOBAI_HAS_PRESET_BASE_URL)
  return preset_nonempty(XIAOBAI_BASE_URL);
#else
  return false;
#endif
}

/// API Key 是否在构建期定死。
[[nodiscard]] constexpr auto api_key_preset_at_build() noexcept -> bool {
#if defined(XIAOBAI_HAS_PRESET_API_KEY)
  return preset_nonempty(XIAOBAI_API_KEY);
#else
  return false;
#endif
}

/// 读环境变量（不存在或空串 → 返回 `fallback`）。
[[nodiscard]] auto env_or(const char* name, const std::string& fallback) -> std::string {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') return fallback;
  return std::string(raw);
}

// ———————————————————————————————————————————————————————————————
// 配置（**纯内存，不落盘**）
//
// 来源只有两个（后者盖前者）：
//   ① **构建期宏**预置（`XIAOBAI_BASE_URL` / `XIAOBAI_API_KEY` / `XIAOBAI_MODEL`）；
//   ② 环境变量（同名）；
//   ③ 设置面板里现填的（仅本次运行有效）。
//
// 为何**不写配置文件**：这个应用的定位是“每次打开都是新的”——
// 不在用户机器上留任何状态文件。因此也没有 `~/.xiaobai/config.json` 这条链；
// 关掉窗口，设置就没了（预置值下次启动会自动回来）。
// ———————————————————————————————————————————————————————————————

/// 预置字符串的取值入口（未预置时返回默认值/空串）。
///
/// 为何包一层而不直接在 `Config` 里写宏：宏可能未定义，
/// 而 `#if` 只能在预处理期分支——包成函数后调用点就干净了。
[[nodiscard]] constexpr auto preset_base_url() noexcept -> const char* {
#if defined(XIAOBAI_HAS_PRESET_BASE_URL)
  return XIAOBAI_BASE_URL;
#else
  return kDefaultBaseUrl;
#endif
}

[[nodiscard]] constexpr auto preset_api_key() noexcept -> const char* {
#if defined(XIAOBAI_HAS_PRESET_API_KEY)
  return XIAOBAI_API_KEY;
#else
  return "";
#endif
}

[[nodiscard]] constexpr auto preset_model() noexcept -> const char* {
#if defined(XIAOBAI_HAS_PRESET_MODEL)
  return XIAOBAI_MODEL;
#else
  return kDefaultModel;
#endif
}

struct Config {
  std::string base_url{base_url_preset_at_build() ? preset_base_url() : kDefaultBaseUrl};
  std::string api_key{api_key_preset_at_build() ? preset_api_key() : std::string{}};
  std::string model{model_preset_at_build() ? preset_model() : kDefaultModel};

  [[nodiscard]] auto ready() const -> bool {
    return !base_url.empty() && !api_key.empty() && !model.empty();
  }
  [[nodiscard]] auto operator==(const Config& other) const -> bool = default;
};

/// 模型名是否已被“定死”（构建期宏或环境变量）。
///
/// 用来决定设置窗要不要暴露模型字段：定死后让用户在界面上改它只会造成
/// “改完不生效”——不如不展示。
[[nodiscard]] auto model_locked() -> bool {
  if (model_preset_at_build()) return true;   // 构建期宏：编译进二进制的值
  const char* raw = std::getenv("XIAOBAI_MODEL");
  return raw != nullptr && *raw != '\0';
}

/// 加载配置：**纯内存**，不读也不写任何磁盘文件。
///
/// 优先级（后者盖前者）：构建期宏预置 → 环境变量。
/// 环境变量盖过宏，是为了让临时调试（换端点/换密钥）不用重编；
/// 生产分发时只给宏、不给环境变量，产物就是开箱即用的。
[[nodiscard]] auto load_config() -> Config {
  Config c;
  c.base_url = env_or("XIAOBAI_BASE_URL", c.base_url);
  c.api_key = env_or("XIAOBAI_API_KEY", c.api_key);
  c.model = env_or("XIAOBAI_MODEL", c.model);
  return c;
}

// ———————————————————————————————————————————————————————————————
// 消息
//
// 一份消息要同时服务两件事：**界面展示**（气泡/推理块/工具卡片）与
// **回传给模型的上下文**（OpenAI `/v1/chat/completions` 的 messages 结构）。
// 这两件事字段并不相同（界面不关心 `tool_call_id`，模型不关心 `stamp`），
// 所以同一个结构里放两套字段，`to_api()` 负责投影出对外的那个。
// ———————————————————————————————————————————————————————————————
struct ToolCall {
  std::string id{};        ///< 模型给的调用 id（回传 `tool` 消息时必须原样带回）
  std::string name{};      ///< 工具名（sh / read / write / edit）
  std::string arguments{}; ///< JSON 字符串（流式下是分片拼起来的）

  [[nodiscard]] auto operator==(const ToolCall& other) const -> bool = default;
};

struct Message {
  /// `user` | `assistant` | `tool`
  std::string role;
  std::string content;
  std::string stamp;  ///< HH:MM（展示用）
  /// 推理过程（`reasoning_content`）：只展示，**不回传给模型**——
  /// DeepSeek / vLLM 系对已完成的推理链不再要求上传，回传反而多花 token。
  std::string reasoning{};
  /// 本消息的思维链是否还在流（决定界面上要不要显示“思考中”）。
  bool reasoning_streaming{false};
  /// 助手要求调用的工具（可能多个，一轮可并行）。
  std::vector<ToolCall> tool_calls{};
  /// 工具结果消息所属的调用 id（`role == "tool"` 时有效）。
  std::string tool_call_id{};
  /// 工具名（`role == "tool"` 时用于展示）。
  std::string tool_name{};
  /// 工具调用入参（JSON 字符串，`role == "tool"` 时用于展示摘要）。
  ///
  /// 为何要存：界面需要告诉用户“这个工具被要求做什么”，
  /// 只有工具名与输出时看不清意图（尤其是 `sh` 跑了什么命令）。
  std::string tool_arguments{};
  /// 工具/命令输出（展示用；可能就是 `content` 的一份拷贝，但两者语义不同）
  std::string output{};
  /// 工具执行是否失败（展示用红色标记）。
  bool tool_failed{false};

  [[nodiscard]] auto operator==(const Message& other) const -> bool = default;
};

// ———————————————————————————————————————————————————————————————
// 后台流式任务
//
// 线程只做三件事：把**已解析的结构化增量**塞进队列（互斥保护），
// 把结束/错误标记置位，把排空的调用信息记下。UI 状态一律由主线程
// （每帧 `pump_stream_into_state`）取走后写——跨线程写 State 会与遍历中的
// 元素树竞争（框架硬约束）。
//
// 为何要把解析放在**后台**：SSE 行可能被 TCP 分片截断，拼行是状态机的事，
// 放主线程每帧做会把“拼到一半的半行”反复搬运。
// ———————————————————————————————————————————————————————————————

/// 一条结构化增量（流式解析的产物）。
struct StreamDelta {
  std::string content{};    ///< 正文增量
  std::string reasoning{};  ///< 推理增量（`reasoning_content`）
  /// 工具调用增量：`index` 是模型给的**序号**（同一工具的分片同名次）；
  /// `id`/`name` 只在首片出现，`args` 是参数 JSON 的分片。
  struct ToolChunk {
    std::size_t index{0};
    std::string id{};
    std::string name{};
    std::string args{};
  };
  std::vector<ToolChunk> tools{};
  /// 本轮的结束原因（`finish_reason`；空 = 还没给）。
  std::string finish_reason{};
};

/// 解析一条 SSE `data:` 行为结构化增量（定义在下面；模板 `pump_stream` 要用）。
[[nodiscard]] auto parse_sse_delta(std::string_view line) -> std::optional<StreamDelta>;

struct StreamJob {
  std::mutex mutex{};
  std::vector<StreamDelta> deltas{};
  std::string error{};
  std::atomic<bool> finished{false};
  std::atomic<bool> cancelled{false};
  std::thread worker{};

  void push(StreamDelta delta) {
    const std::lock_guard<std::mutex> guard(mutex);
    deltas.push_back(std::move(delta));
  }
  void fail(std::string message) {
    const std::lock_guard<std::mutex> guard(mutex);
    if (error.empty()) error = std::move(message);
  }
  /// 取走已收增量（错误也一并通过 `error` 字段返回）。
  void drain(std::vector<StreamDelta>& out, std::string& out_error) {
    const std::lock_guard<std::mutex> guard(mutex);
    out.insert(out.end(), std::make_move_iterator(deltas.begin()),
               std::make_move_iterator(deltas.end()));
    deltas.clear();
    out_error = error;
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
  // HTTP 头与正文的分界：头里有 `Transfer-Encoding: chunked` 时正文是
  // “十六进制长度行 + 数据”的分块格式，必须剥掉否则 SSE 行会被长度行切断。
  bool headers_done = false;
  bool chunked = false;
  // 分块解码的剩余字节数（0 = 等下一块的长度行）。
  std::size_t chunk_left = 0;
  while (!job.cancelled.load()) {
    auto read = stream.read_some(std::span<std::uint8_t>(buf.data(), buf.size()));
    if (!read.has_value()) {
      job.fail("读取失败：" + read.error().message);
      return;
    }
    if (*read == 0) break;  // 对端关闭 = 正常结束
    pending.append(reinterpret_cast<const char*>(buf.data()), *read);

    if (!headers_done) {
      const auto split = pending.find("\r\n\r\n");
      if (split == std::string::npos) continue;   // 头还没收齐
      chunked = pending.substr(0, split).find("chunked") != std::string::npos;
      pending.erase(0, split + 4);
      headers_done = true;
    }

    // —— 分块解码（先剥壳再按行切 SSE）——
    if (chunked) {
      std::string body;
      while (true) {
        if (chunk_left == 0) {
          const auto line_end = pending.find("\r\n");
          if (line_end == std::string::npos) break;
          std::size_t size = 0;
          try {
            size = static_cast<std::size_t>(
                std::stoul(pending.substr(0, line_end), nullptr, 16));
          } catch (const std::exception&) {
            pending.clear();
            break;
          }
          pending.erase(0, line_end + 2);
          if (size == 0) { pending.clear(); break; }   // 末分块
          chunk_left = size;
        }
        if (pending.size() < chunk_left) break;   // 半块：等更多数据
        body.append(pending, 0, chunk_left);
        pending.erase(0, chunk_left);
        chunk_left = 0;
        // 块尾的 `\r\n`
        if (pending.size() >= 2 && pending.compare(0, 2, "\r\n") == 0) pending.erase(0, 2);
      }
      pending = std::move(body) + pending;
    }

    // —— 按行切 SSE（一条 `data:` 行 = 一个 delta）——
    std::size_t line_start = 0;
    while (true) {
      const auto line_end = pending.find('\n', line_start);
      if (line_end == std::string::npos) break;
      std::string line = pending.substr(line_start, line_end - line_start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      line_start = line_end + 1;
      if (auto delta = parse_sse_delta(line); delta.has_value()) {
        if (delta->finish_reason == "done") break;   // `[DONE]`
        job.push(std::move(*delta));
      }
    }
    pending.erase(0, line_start);
  }
}

// ———————————————————————————————————————————————————————————————
// 模型列表拉取（`GET /v1/models`）
//
// 与流式对话同样跑在后台线程：UI 线程只管每帧把结果取走。
// 为什么这个要单独一个类型而不复用 `StreamJob`：它要的是**一次性收齐**
// （典型响应几百字节），且必须等到完整报文才能解析——不需要增量。
// ———————————————————————————————————————————————————————————————
struct ModelListJob {
  std::mutex mutex{};
  std::vector<std::string> models{};
  std::string error{};
  std::atomic<bool> finished{false};
  std::thread worker{};

  [[nodiscard]] auto drain(std::vector<std::string>& out_models, std::string& out_error) -> bool {
    if (!finished.load()) return false;
    const std::lock_guard<std::mutex> guard(mutex);
    out_models = models;
    out_error = error;
    return true;
  }
};

template <class Stream>
void fetch_model_list(Stream& stream, ModelListJob& job, const std::string& wire) {
  if (auto status = stream.write_all(std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()));
      !status) {
    const std::lock_guard<std::mutex> guard(job.mutex);
    job.error = "发送请求失败：" + status.error().message;
    job.finished.store(true);
    return;
  }
  std::vector<std::uint8_t> buf(16 * 1024);
  std::string raw;
  while (true) {
    auto read = stream.read_some(std::span<std::uint8_t>(buf.data(), buf.size()));
    if (!read.has_value()) {
      const std::lock_guard<std::mutex> guard(job.mutex);
      if (job.error.empty()) job.error = "读取失败：" + read.error().message;
      break;
    }
    if (*read == 0) break;
    raw.append(reinterpret_cast<const char*>(buf.data()), *read);
  }
  // 拆头部/正文：兼容 `\r\n\r\n` 与 `\n\n` 两种分隔。
  std::string body;
  const auto split = raw.find("\r\n\r\n");
  if (split != std::string::npos) {
    body = raw.substr(split + 4);
  } else if (const auto alt = raw.find("\n\n"); alt != std::string::npos) {
    body = raw.substr(alt + 2);
  }
  // 分块传输（`Transfer-Encoding: chunked`）：只取十六进制长度行之后的载荷。
  if (raw.find("chunked") != std::string::npos) {
    std::string joined;
    std::size_t at = 0;
    while (at < body.size()) {
      const auto line_end = body.find("\r\n", at);
      if (line_end == std::string::npos) break;
      std::size_t size = 0;
      try {
        size = static_cast<std::size_t>(std::stoul(body.substr(at, line_end - at), nullptr, 16));
      } catch (const std::exception&) {
        break;
      }
      if (size == 0) break;
      const std::size_t from = line_end + 2;
      joined.append(body.substr(from, std::min(size, body.size() - from)));
      at = from + size + 2;
    }
    body = std::move(joined);
  }
  {
    const std::lock_guard<std::mutex> guard(job.mutex);
    if (job.error.empty()) {
      const auto parsed = st::json_parse(body);
      if (!parsed.has_value()) {
        job.error = "模型列表响应不是合法 JSON";
      } else {
        const Json* data = st::json_find(*parsed, "data");
        if (data == nullptr || !data->is_array()) {
          job.error = "模型列表响应缺少 data 数组";
        } else {
          for (const auto& item : *data) {
            const Json* id = st::json_find(item, "id");
            if (id != nullptr && id->is_string()) {
              job.models.push_back(st::json_as_string(*id));
            }
          }
          std::ranges::sort(job.models);
          if (job.models.empty()) job.error = "该端点未返回任何模型";
        }
      }
    }
  }
  job.finished.store(true);
}

/// 构造模型列表请求（`GET {base}/models`，同基础地址与认证）。
[[nodiscard]] auto build_models_request(const std::string& base_url, const std::string& api_key,
                                        std::string& out_url) -> std::string {
  std::string url = base_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  // 用户可能填的是完整 chat/completions：从那里截到根再拼 models。
  const auto chat_at = url.find("/chat/");
  if (chat_at != std::string::npos) url.resize(chat_at);
  if (url.find("/models") == std::string::npos) url += "/models";
  out_url = url;

  http::HttpRequest req;
  req.method = http::Method::Get;
  const auto parsed = http::Url::parse(url);
  req.path = (parsed.has_value() && !parsed->path.empty()) ? parsed->path : "/v1/models";
  req.host = parsed.has_value() ? parsed->host : std::string{};
  req.port = parsed.has_value() ? parsed->port : 80;
  req.headers.set("Authorization", "Bearer " + api_key);
  req.headers.set("Accept", "application/json");
  req.headers.set("Connection", "close");
  return http::serialize_request(req);
}

/// 把 URL 与请求体拼成 OpenAI 兼容的完整报文。
///
/// `tools` 非空时带上工具声明（模型据此才会返回 `tool_calls`）。
[[nodiscard]] auto build_request(const Config& cfg, const std::vector<Json>& messages,
                                 const Json& tools, std::string& out_url) -> std::string {
  std::string url = cfg.base_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  // 端点已含路径就不再拼（允许用户直接填完整的 .../chat/completions）
  if (url.find("/chat/") == std::string::npos) url += "/chat/completions";
  out_url = url;

  Json payload = Json::object();
  payload["model"] = cfg.model;
  payload["messages"] = messages;
  payload["stream"] = true;
  // `tools` 是数组；空数组不要发（部分网关对空数组报 400）。
  if (tools.is_array() && !tools.empty()) payload["tools"] = tools;
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

/// 解析一条 SSE `data:` 行为结构化增量。
///
/// `[DONE]` 以 `finish_reason = "done"` 告知调用方（而不是返回 nullopt——
/// nullopt 同时表示“这行不是数据行”，两者必须可分辨）。
[[nodiscard]] auto parse_sse_delta(std::string_view line) -> std::optional<StreamDelta> {
  if (!line.starts_with("data:")) return std::nullopt;
  std::string_view data = line.substr(5);
  while (!data.empty() && (data.front() == ' ' || data.front() == '\t')) data.remove_prefix(1);
  if (data.empty()) return std::nullopt;
  if (data == "[DONE]") {
    StreamDelta done;
    done.finish_reason = "done";
    return done;
  }
  const auto parsed = st::json_parse(data);
  if (!parsed.has_value()) return std::nullopt;
  const Json* choices = st::json_find(*parsed, "choices");
  if (choices == nullptr || !choices->is_array() || choices->empty()) return std::nullopt;
  const Json& choice = (*choices)[0];

  StreamDelta out;
  if (const Json* reason = st::json_find(choice, "finish_reason");
      reason != nullptr && reason->is_string()) {
    out.finish_reason = st::json_as_string(*reason);
  }

  const Json* delta = st::json_find(choice, "delta");
  if (delta == nullptr) return out;

  // 正文
  if (const Json* content = st::json_find(*delta, "content");
      content != nullptr && content->is_string()) {
    out.content = st::json_as_string(*content);
  }
  // 推理过程：DeepSeek / vLLM 用 `reasoning_content`，部分网关用 `reasoning`。
  for (const char* key : {"reasoning_content", "reasoning"}) {
    if (const Json* reasoning = st::json_find(*delta, key);
        reasoning != nullptr && reasoning->is_string()) {
      out.reasoning = st::json_as_string(*reasoning);
      break;
    }
  }
  // 工具调用分片
  if (const Json* calls = st::json_find(*delta, "tool_calls");
      calls != nullptr && calls->is_array()) {
    for (const auto& call : *calls) {
      StreamDelta::ToolChunk chunk;
      if (const Json* index = st::json_find(call, "index");
          index != nullptr && index->is_number()) {
        chunk.index = static_cast<std::size_t>(st::json_as_i64(*index));
      }
      if (const Json* id = st::json_find(call, "id"); id != nullptr && id->is_string()) {
        chunk.id = st::json_as_string(*id);
      }
      if (const Json* fn = st::json_find(call, "function"); fn != nullptr) {
        if (const Json* name = st::json_find(*fn, "name");
            name != nullptr && name->is_string()) {
          chunk.name = st::json_as_string(*name);
        }
        if (const Json* args = st::json_find(*fn, "arguments");
            args != nullptr && args->is_string()) {
          chunk.args = st::json_as_string(*args);
        }
      }
      out.tools.push_back(std::move(chunk));
    }
  }
  return out;
}

// ———————————————————————————————————————————————————————————————
// 工具（sh / read / write / edit）
//
// 四个工具都是「模型说要做什么 → 我们在本机做 → 把结果成文本回传」。
// 设计约束：
//
// - **共享一个工作目录**（`tool_root_`）：应用当前目录。模型给的路径一律
//   当作相对它解析——"读 project/a.txt" 与用户心里的那个位置一致。
// - **输出有上限**：模型上下文很贵，一个 `ls -R /` 能直接撑爆。超限截断并
//   明确告知（截断标记本身就是给模型的信息）。
// - **失败不抛异常**：工具错也是给模型的输入（它要据此改正），
//   所以统一返回 `ToolResult{ok, text}` 而不是错误码。
// - **不弹控制台**：`sh` 走捕获管道（`capture_output`），窗口应用无控制台。
// ———————————————————————————————————————————————————————————————

/// 工具执行结果。
struct ToolResult {
  bool ok{false};
  std::string text{};   ///< 回传给模型的内容（已截断到上限）
};

/// 单个工具输出的字符上限。超了截断并注明——
/// 不截断的代价是上下文被一条命令撑满（实测 `ls -R` 很容易几十万字符）。
inline constexpr std::size_t kToolOutputLimit = 16 * 1024;
/// `sh` 命令超时（毫秒）：防一条挂住的命令卡住整个会话。
inline constexpr int kToolShellTimeoutMs = 60 * 1000;

[[nodiscard]] auto truncate_tool_output(std::string text) -> std::string {
  if (text.size() <= kToolOutputLimit) return text;
  const std::size_t kept = text.size() - kToolOutputLimit;
  text.resize(kToolOutputLimit);
  text += "\n…（输出过大，已截断 " + std::to_string(kept) + " 字符）";
  return text;
}

/// 解析工具参数（模型给的是 JSON 字符串）。
[[nodiscard]] auto tool_arg(const Json& args, std::string_view key) -> std::string {
  const Json* value = st::json_find(args, std::string(key));
  if (value == nullptr || !value->is_string()) return {};
  return st::json_as_string(*value);
}

/// 把相对路径解析到工具根目录（绝对路径原样返回）。
[[nodiscard]] auto resolve_tool_path(const std::string& root, const std::string& path)
    -> std::string {
  if (path.empty()) return root;
  if (path.find(":\\") != std::string::npos || path.find(":/") != std::string::npos ||
      (!path.empty() && (path.front() == '/' || path.front() == '\\'))) {
    return path;   // 已经是绝对路径
  }
  return st::fs::join(root, path);
}

/// `edit`：把文件里的 `old` 全数替换为 `new`。
///
/// 为何要强制“必须命中”：常见的失败是模型猜了不存在的原文，
/// 而默默写回一份未改动的文件会让它以为改成功了（这是最贵的失败模式）。
[[nodiscard]] auto tool_edit_text(const std::string& path, const std::string& old_text,
                                  const std::string& new_text) -> ToolResult {
  auto loaded = st::fs::read_text(path);
  if (!loaded.has_value()) {
    return ToolResult{false, "读取失败：" + loaded.error().message + "（路径：" + path + "）"};
  }
  std::string content = std::move(*loaded);
  if (old_text.empty()) return ToolResult{false, "old_string 不能为空"};
  std::size_t count = 0;
  std::size_t at = content.find(old_text);
  if (at == std::string::npos) {
    return ToolResult{false,
                      "未找到要替换的内容（文件未改动）。请先用 read 确认原文。路径：" + path};
  }
  while (at != std::string::npos) {
    content.replace(at, old_text.size(), new_text);
    ++count;
    at = content.find(old_text, at + new_text.size());
  }
  if (auto status = st::fs::write_text(path, content); !status) {
    return ToolResult{false, "写入失败：" + status.error().message};
  }
  return ToolResult{true, "已替换 " + std::to_string(count) + " 处。文件：" + path};
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
  /// 生成中 / 出错的提示；**空串 = 不显示**（就绪态一律为空，界面上没有状态行）。
  st::ui::dsl::State<std::string> status{""};
  st::ui::dsl::State<bool> settings_open{false};
  st::ui::dsl::State<Config> config{load_config()};
  // 设置弹窗的编辑缓冲（打开时从 config 拷贝，保存时写回）
  st::ui::dsl::State<std::string> draft_url{};
  st::ui::dsl::State<std::string> draft_key{};
  st::ui::dsl::State<std::string> draft_model{};

  /// 真值树根（下拉面板需要挂叠加层）；由 main 在 mount 时注入。
  st::ui::UiRoot* root_host{nullptr};

  /// 工具的工作目录（模型给的相对路径一律相对它）。main 在启动时设成进程当前目录。
  std::string tool_root{};
  /// 当前是否允许使用工具（界面上可关——关掉就是一个纯聊天窗）。
  st::ui::dsl::State<bool> tools_enabled{true};
  /// 当前这一轮已经跑了多少个工具调用（防无限循环的硬护栏）。
  std::size_t tool_rounds{0};

  /// 单会话最多几轮「模型要工具 → 我们执行 → 回传」。
  ///
  /// 上限不是为了省，而是防模型卡在“重复调同一个失败工具”的循环里（实测常见）。
  /// 到了就停下来并把控制交回用户，而不是默默转下去。
  static constexpr std::size_t kMaxToolRounds = 32;

  /// 当前请求（非空 = 正在流式生成）。
  std::unique_ptr<StreamJob> job{};
  /// 模型列表拉取（非空 = 正在拉）。
  std::unique_ptr<ModelListJob> models_job{};
  /// 拉回来的可选模型（供设置面板的模型下拉用）。
  st::ui::dsl::State<std::vector<std::string>> model_options{std::vector<std::string>{}};
  /// 拉取中的提示（空 = 空闲）；展示在模型字段旁。
  st::ui::dsl::State<std::string> models_hint{""};
  /// 标题栏上的模型标签（非本页声明树里的元素，由 main 装配时建）。
  /// 每帧由 `sync_title_badge()` 把当前模型写进去。
  st::ui::Element* model_badge{nullptr};
  /// 流式输出累积的正文（主线程侧，属于当前助手消息）。
  std::string stream_text{};
  /// 流式累积的推理过程（同上，只展示）。
  std::string stream_reasoning{};
  /// 本轮已拼好的工具调用（流式分片按 `index` 合并到这里）。
  std::vector<ToolCall> pending_calls{};

  AgentPage() {
    // 首条欢迎消息。提示语只说“做什么”，不提已经不存在的入口
    //（旧文案让人去「设置」里填，而那个按钮已被删掉）。
    messages.set(std::vector<Message>{
        Message{"assistant", "你好，我是小白。有什么需要帮助的吗？", ""}});
  }

  // —— 声明式构建 ——
  void build(st::ui::dsl::Composer& c) override {
    using namespace st::ui::dsl;

    (void)column(c, {.gap = 0, .grow = true, .key = "root"}, [&] {
      // 不再有顶栏：模型名放**标题栏**（`TitleBar::add_trailing`，见 main 的装配），
      // 状态信息（生成中/出错）放在消息流末尾。这样聊天区从窗口顶部直接开始，
      // 少一条常驻横带——与歌白主界面同一取向。

      // ── 消息区 ──
      //
      // 底部留白 = 胶囊高度 + 两侧边距，否则最后一条消息会被悬浮胶囊压住。
      auto& scroll = custom<st::ui::ScrollView>(
          c,
          [](st::ui::ScrollView& s) {
            s.set_id("msg-scroll");
            s.set_show_scrollbar(true);
            // **页面底渐变交给主题令牌**（`Style::use_theme_backdrop`）。
            //
            // 为何不能省（实测）：背后是纯色时，气泡模糊/折射的结果与纯色几乎一致，
            // 看上去就是一块“淡了一点的灰”——用户会认为玻璃没生效。
            //
            // 为何不在此写色泽：层次颜色必须与亮/暗主题**同时**成立，写死一组
            // 暖→冷色，换到暗色主题就是一层灰蒙；而且 `build` 在换肤时不会重跑，
            // 写了也拿不到新主题。声明这个标志后，颜色由 `refresh_theme` 同步。
            s.style().use_theme_backdrop = true;
            // **跟随底部**：流式输出/新消息到达时自动跟到底。
            //
            // 为何不自己 `scroll_to(1e9)`（`set_follow_end` 就是为此而存在）：
            // `max_scroll()` 取的是**上一次布局**的 `content_height_`，刚追加就滚
            // 会被旧上限夹住；等布局把内容量成新值、重新夹取时，那个偏移已经
            // 变成“合法”了——于是每次都差新长出来的那几行
            // （实测：长回答的末尾永远看不到）。
            s.set_follow_end(true);
            // 用户往上翻 ⇒ 关掉跟随（否则每长出一行就把他拉回底部）；
            // 自己滚回底部 ⇒ 重新开启。回调拿到的是**新偏移**，与 `max_scroll()`
            // 一比就知道在不在底部。
            st::ui::ScrollView* self = &s;
            s.set_on_scroll([self](float offset) {
              const bool at_bottom = offset >= self->max_scroll() - 2.0f;
              if (self->follow_end() != at_bottom) self->set_follow_end(at_bottom);
            });
          },
          {.grow = true, .key = "msg-scroll"});
      {
        BuildScope scope(c, &scroll);
        (void)column(c, {.gap = 10, .padding_x = 14, .padding_y = 14,
                         .key = "msg-list"}, [&] {
          const auto all = messages.value();
          for (std::size_t i = 0; i < all.size(); ++i) bubble(c, all[i], i);
          // 生成/错误状态：**只在非空时出现**（就绪是常态，不该占一行）。
          // 正常态下这里什么都不声明，于是界面里没有状态条。
          if (!status.value().empty()) {
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

  /// 一条消息：用户靠右气泡、助手靠左（推理块 + 正文）、工具是独立的小卡片。
  ///
  /// 三类消息形态差别很大，所以不做一个万能气泡：
  /// - `user`：普通文本气泡（靠右）
  /// - `assistant`：可选推理块（暗色、等宽）+ Markdown 正文
  /// - `tool`：工具名 + 成败标记 + 等宽输出（截断展示）
  /// `index` 是**消息在列表里的位置**，用来生成唯一 key。
  ///
  /// 为何必须唯一（实测事故）：同一 role 的多条消息若共用 `body-assistant`
  /// 这类固定 key，声明式对账会按 key 复用元素——**后一条把前一条的元素偷走**，
  /// 于是“读了 A 的内容、显示了 B 的位置”。表现极像渲染 bug（高度/内容对不上），
  /// 而根因在 key 冲突。`row-` 那层之前还用 `messages.size()` 当后缀，
  /// 消息变多时同一个 key 会指向不同位置，同样不稳定。
  void bubble(st::ui::dsl::Composer& c, const Message& m, std::size_t index) {
    using namespace st::ui::dsl;
    if (m.role == "tool") {
      tool_card(c, m, index);
      return;
    }
    const bool is_user = m.role == "user";
    const std::string uid = std::to_string(index);
    // **完全空白的助手消息不画气泡**。
    //
    // 什么时候会出现：模型一轮只发工具调用、没写任何正文（它只“想了”+“调工具”），
    // 而应用会为下一轮预建一个空助手消息。旧实现照样画出气泡骨架，
    // 屏幕上就是一个只有“AI 14:31”的**空气泡**（实测截图里能看到）。
    // 用户消息不适用此规则：空输入的 user 消息本来就不会产生。
    if (!is_user && m.content.empty() && m.reasoning.empty()) return;
    // **推理过程单独一个气泡**（不埋在答复气泡里）。
    //
    // 为何要拆开（用户直接提的诉求）：推理是**过程**、答复是**结果**，
    // 两者混在同一张卡片里时：“这段是模型说的还是模型想的”分不清，
    // 且推理很长时把答复挤到看不见的地方。它们是两个消息主体，
    // 宽度也该各算各的（推理往往比答复长得多）。
    if (!is_user && !m.reasoning.empty()) reasoning_bubble(c, m, index);
    // **用 `justify` 对齐，不用 `spacer`**：
    //
    // `spacer` 是一个 `grow` 的空元素，它会把行内的剩余宽**全部占掉**——
    // 气泡于是永远只能长到“行宽的一半”左右（实测：窗口 1178、气泡封顶 520）。
    // 而两侧留白应该由消息区自身的 `padding_x` 提供（那是窗口边距，要保留），
    // 对齐则只是“靠左/靠右”的位置问题，两者不该用同一个机制。
    (void)row(c, {.gap = 0, .max_height = st::ui::kUnbounded,
                  .justify = is_user ? st::ui::Justify::End : st::ui::Justify::Start,
                  .key = "row-" + m.role + "-" + uid},
              [&] {
                // **`elevation = Flat` 是必需项，不是修饰**：
                // `Card` 的投影默认是 `shadow_level_ = 2`（md），而它画在**玻璃底下**——
                // 背板模糊会把投影平均进来，整块气泡被压暗（实测：气泡内 200.7
                // vs 背景 239.7，暗 39；关掉后气泡内 243.0、反推背板 239.6）。
                // 玻璃本身就是“浮起来”的材质，再加投影既不符语义、又把通透感吃掉。
                (void)card(c, {.gap = 6, .padding = 10, .max_width = kMaxBubbleWidth,
                               .max_height = st::ui::kUnbounded,
                               .radius = kGlassRadius,
                               // **不叠 `surface`**：它是主题实色底（`Base` = #FEFEFF、
                               // `Sunken` = #E9EAF0 灰），会带自己的色相与亮度盖在背板上，
                               // 玻璃变成“罩了一层雾的卡片”。
                               //
                               // 底色与 `tint` 都**交给主题档位**（`theme_glass`）：
                               // 亮色用纯白 + tint 0.22；暗色必须换成淡白（α≈18%）+
                               // tint 0.42，否则同一组参数会把暗色气泡抹成发辉的灰纸
                               //（实测：沿用亮色档时气泡内 73.7 vs 背景 23.0，亮 50）。
                               // **玻璃元素不画描边**：边框由 `top_highlight` 承担。
                               //
                               // 实测（用户标注截图，亮色 1988px 宽的对话项）：
                               // 带 `border_color = 白 α140` 时，顶边会出现一条 **1px 高、
                               // 亮度恒为 246.0** 的直线（从 x=40 一直拉到 x=1920，逐点完全相等），
                               // 而上下两侧是 228~236——没有任何渐变，看起来就是“多了一道边框”。
                               // 原因：环形描边在 DPI 1.25 下是全宽等亮的，它不会像高光那样衰减；
                               // 而高光已经在做“边缘受光”，两者叠在一起就是两道平行亮线。
                               //
                               // 高光宽度不是“玻璃强度”而是几何（亮带的厚），
                               // 不进 `glass` 字符串（那里管的是强度量）。
                               .highlight_width = 1.5f,
                               // **一行字符串配置玻璃**（与 XML 的 `glass="…"` 同一语法）：
                               // 四个参数各自一个字段时要盯四处，漏一个就静默半开
                               //（只给 blur 忘了 tint = “模糊但实心”）。
                               .glass = kGlassSpec,
                               .theme_glass = true,
                               .elevation = st::ui::Elevation::Flat,
                               .key = "bubble-" + m.role + "-" + uid},
                           [&] {
                             (void)row(c, {.gap = 6, .key = "meta"}, [&] {
                               (void)icon(c, is_user ? "user" : "sparkles", 12,
                                          {.color = is_user ? st::ui::Tone::Muted
                                                            : st::ui::Tone::Accent,
                                           .key = "meta-icon"});
                               (void)text(c, [&] { return is_user ? std::string("你") : std::string("AI"); },
                                          {.size = 11.0f, .weight = st::ui::FontWeight::Bold,
                                           .key = "meta-name"});
                               // 推理中/已推理的标记：有推理内容就亮一个“思考”标签，
                               // 流式期间文字是“思考中”，结束后变“已思考”。
                               if (!is_user && !m.reasoning.empty()) {
                                 (void)text(
                                     c,
                                     [&]() -> std::string {
                                       return m.reasoning_streaming ? std::string("思考中…")
                                                                    : std::string("已思考");
                                     },
                                     {.color = st::ui::Tone::Accent, .size = 10.0f,
                                      .key = "reasoning-badge"});
                               }
                               if (!m.stamp.empty()) {
                                 (void)text(c, [&]() -> std::string { return m.stamp; },
                                            {.color = st::ui::Tone::Muted, .size = 10.0f,
                                             .key = "meta-time"});
                               }
                             });
                             // 正文：用 `custom<MarkdownView>` 而不 `markdown()`：需要
                             // “按内容收缩宽度”（气泡随短消息变窄）。
                             // 代价：`markdown()` 的惰性 source 闭包（自动订阅）要手动补——
                             // 先在该帧读一次 State 登记依赖，再把值写进去。
                             const std::string body = m.content;   // 读值 = 登记依赖
                             if (!body.empty()) {
                               (void)custom<st::ui::MarkdownView>(
                                   c,
                                   [body](st::ui::MarkdownView& view) {
                                     view.set_shrink_to_fit(true);
                                     view.set_base_font_size(13.0f);
                                     // `set_markdown` 自带相等早退，逐帧重设不贵。
                                     view.set_markdown(body);
                                   },
                                   // **`max_height` 必须显式给无界**：气泡里的正文
                                   // 要**全部展开**（长回答不能内部滚动），而默认继承的
                                   // 约束会让它停在某个高度、内部出滚动条——
                                   // 表现就是“最后一段被切掉”（实测：内容 297px、
                                   // 只给 235px，剩下 62px 得内滚才看得到）。
                                   {.max_height = st::ui::kUnbounded,
                                    .key = "body-" + m.role + "-" + uid});
                             }
                           });
              });
  }

  /// 推理过程的**独立气泡**（与答复气泡平级，靠在左）。
  ///
  /// 为何单独一个气泡而不塞进答复卡片：推理是**过程**、答复是**结果**。
  /// 混在一起时既分不清“这句是说的还是想的”，推理很长时还会把答复顶到
  /// 看不见的地方；分开后两者各自自适应宽度（推理通常比答复长很多）。
  ///
  /// 视觉上弱于正文（暗底 + 小一号字），因为它是背景信息而不是回答。
  void reasoning_bubble(st::ui::dsl::Composer& c, const Message& m, std::size_t index) {
    using namespace st::ui::dsl;
    const std::string uid = std::to_string(index);
    const std::string trace = m.reasoning;
    (void)row(c, {.gap = 0, .max_height = st::ui::kUnbounded,
                  .justify = st::ui::Justify::Start,
                  .key = "row-reasoning-" + uid},
              [&] {
                // **推理块不用 `card()` 而要 `custom_container<Card>`**：
                // 需要关掉 `Card` 的默认投影（`shadow_level_ = 2`，md），而
                // `BoxProps` 没有对应字段（`elevation = Flat` 会同时把底重置成主题取色，
                // 覆盖掉下面要的 `Sunken`）。
                //
                // 为何必须关（用户原话“为啥只有上边框有”，附截图实测）：
                // 卡片上缘 y=662 有一行**横贯全宽**（x=100..2300）的硬线，
                // 亮度 197（背景 235、卡内深处 242）。那是 md 投影落在卡片
                // 上缘的那一圈暗晕——投影是**对称外扩**的，但卡片下方是内容、
                // 左右是空白，只有上缘紧贴一段浅色背景，所以只有它被读成“一条边”。
                // 二维渐变带（`inset`）不是这里的原因：本卡片的 shadow 是外投影。
                (void)custom_container<st::ui::Card>(
                    c,
                    [&] {
                             (void)row(c, {.gap = 5, .align_items = st::ui::Align::Center,
                                           .key = "reasoning-meta-" + uid}, [&] {
                               (void)icon(c, "sparkles", 11,
                                          {.color = st::ui::Tone::Muted,
                                           .key = "reasoning-icon-" + uid});
                               (void)text(
                                   c,
                                   [&]() -> std::string {
                                     return m.reasoning_streaming ? "思考中…" : "思考过程";
                                   },
                                   {.color = st::ui::Tone::Muted, .size = 10.5f,
                                    .key = "reasoning-title-" + uid});
                             });
                             (void)custom<st::ui::MarkdownView>(
                                 c,
                                 [trace](st::ui::MarkdownView& view) {
                                   view.set_shrink_to_fit(true);
                                   view.set_base_font_size(12.0f);
                                   view.set_markdown(trace);
                                 },
                                 {.max_height = st::ui::kUnbounded,
                                  .key = "reasoning-" + uid});
                    },
                    [](st::ui::Card& card) {
                      // 关掉默认 md 投影：它落在卡片上缘会形成一道暗晕。
                      card.set_shadow_level(0);
                    },
                    {.gap = 4,
                     .padding = 9,
                     .max_width = kMaxBubbleWidth,
                     .max_height = st::ui::kUnbounded,
                     .radius = kGlassRadius,
                     .surface = st::ui::Element::Surface::Sunken,
                     // **不画描边**（用户原话：“不想要这条怪怪的边”）。
                     //
                     // 实测：关掉投影后上缘仍剩 y=156 处的一道 **222**（上下 239/235）。
                     // 那是 `Card::apply_theme` 给的 1px 描边（黑 5.5%）——
                     // **必须走声明式字段**，在 `custom_container` 的 configure 回调里
                     // 写 `card.style().border_width = 0` 会被之后跑的主题重算盖回去
                     //（实测改了三次数值完全不变，就是这个原因）。
                     .border_width = 1.0f,
                     .border_color = st::math::Color{0, 0, 0, 1},
                     .key = "reasoning-block-" + uid});
              });
  }

  /// 工具名的**中文显示名**（界面上给人看的）。
  ///
  /// 为何要映射而不是直接用原名：`sh` / `read` / `write` / `edit` 是给模型看的
  /// 函数名，直接当标题显示对用户没有任何含义。未登记的名字原样返回
  /// （新增工具时忘补映射也不至于界面上凭空消失）。
  [[nodiscard]] static auto tool_label(std::string_view name) -> std::string {
    if (name == "sh") return "执行命令";
    if (name == "read") return "读取文件";
    if (name == "write") return "写入文件";
    if (name == "edit") return "修改文件";
    return std::string(name);
  }

  /// 把工具入参摊成一行可读摘要（“执行命令 · `npm test`”的那种）。
  ///
  /// 为何要摘要而不直接打整份 JSON：���数里常带整个文件内容（`write`/`edit`），
  /// 原样展示会把卡片撑成一面墙。只需让人看出“对它做了什么”。
  [[nodiscard]] static auto tool_arguments_brief(std::string_view name, std::string_view arguments)
      -> std::string {
    // 多行内容在摘要里压成一行（否则首行之后的内容全被截掉，看不出参数是谁）。
    const auto one_line = [](std::string_view text) {
      std::string out;
      out.reserve(text.size());
      for (const char ch : text) out.push_back(ch == '\n' || ch == '\r' ? ' ' : ch);
      return out;
    };
    // **失败也要给出原文**：参数解析不出来时，让用户看到模型到底传了什么
    // （否则只能看到“参数无效”却不知道哪里错）。
    const auto parsed = st::json_parse(arguments);
    if (!parsed.has_value()) return one_line(arguments);
    const Json& args = *parsed;
    // 每个工具只抽“最能说明动作”的那一两个字段。
    std::string brief;
    if (name == "sh") {
      brief = tool_arg(args, "command");
    } else if (name == "read" || name == "write") {
      brief = tool_arg(args, "path");
    } else if (name == "edit") {
      const std::string path = tool_arg(args, "path");
      const std::string from = tool_arg(args, "old_string");
      brief = path;
      if (!from.empty()) brief += "  「" + from + "」";
    } else {
      // 未知工具：把整个 JSON 压成一行（总比什么都没有强）。
      brief = one_line(arguments);
    }
    if (brief.size() > 160U) {
      brief.resize(160U);
      brief += "…";
    }
    return one_line(brief);
  }

  /// 工具调用结果卡片：图标 + 中文工具名 + 参数摘要 + 输出（等宽代码块）。
  ///
  /// 输出截到 1200 字符只是为了**界面**不被一条日志撑爆；回传给模型的
  /// 内容仍是完整的（已由 `kToolOutputLimit` 控住）。
  void tool_card(st::ui::dsl::Composer& c, const Message& m, std::size_t index) {
    using namespace st::ui::dsl;
    const std::string uid = std::to_string(index);
    std::string preview = m.output;
    if (preview.size() > 1200) {
      preview.resize(1200);
      preview += "\n…（界面预览已截断）";
    }
    const std::string label = tool_label(m.tool_name);
    const std::string brief = tool_arguments_brief(m.tool_name, m.tool_arguments);
    (void)row(c, {.gap = 0, .max_height = st::ui::kUnbounded,
                  .justify = st::ui::Justify::Start,
                  .key = "row-tool-" + uid}, [&] {
      (void)card(c,
                 {.gap = 5, .padding = 9, .max_width = kMaxBubbleWidth,
                  // 与气泡同理：工具输出也全高展开（外层消息区滚）。
                  .max_height = st::ui::kUnbounded,
                  .surface = st::ui::Element::Surface::Sunken,
                  .key = "tool-" + m.tool_name + "-" + uid},
                 [&] {
                   (void)row(c, {.gap = 6, .align_items = st::ui::Align::Center,
                                 .key = "tool-meta"}, [&] {
                     (void)icon(c, m.tool_failed ? "error" : "check", 12,
                                {.color = m.tool_failed ? st::ui::Tone::Danger
                                                        : st::ui::Tone::Success,
                                 .key = "tool-icon"});
                     // 标题用**中文工具名**（`sh` → “执行命令”）：给用户看的是意图。
                     (void)text(c, [label] { return label; },
                                {.size = 11.5f, .weight = st::ui::FontWeight::Bold,
                                 .key = "tool-name"});
                     (void)text(c, [&] { return m.tool_failed ? "失败" : "完成"; },
                                {.color = st::ui::Tone::Muted, .size = 10.0f,
                                 .key = "tool-state"});
                   });
                   // 执行参数摘要：让用户看出“对它做了什么”。
                   if (!brief.empty()) {
                     (void)text(c, [brief] { return brief; },
                                {.color = st::ui::Tone::Muted, .size = 10.5f,
                                 .key = "tool-args"});
                   }
                   if (!preview.empty()) {
                     (void)custom<st::ui::MarkdownView>(
                         c,
                         [preview](st::ui::MarkdownView& view) {
                           view.set_shrink_to_fit(true);
                           view.set_base_font_size(11.0f);
                           // 包成代码块：工具输出不该被当成 Markdown 渲染（缩进/`*` 会变形）。
                           view.set_markdown("```\n" + preview + "\n```");
                         },
                         // 与气泡正文同理：内容全高展开，不在卡片内出滚动条。
                         {.max_height = st::ui::kUnbounded,
                          .key = "tool-output-" + uid});
                   }
                 });
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

                 settings_field(c, "Base URL", "", draft_url.value(),
                                [this](std::string v) { draft_url.set(std::move(v)); },
                                "cfg-url");
                 settings_field(c, "API Key", "", draft_key.value(),
                                [this](std::string v) { draft_key.set(std::move(v)); },
                                "cfg-key", /*password=*/true);
                 // 模型字段：输入框内嵌下拉按钮（框架 `Input` 的能力）。
                 model_field(c);

                 (void)row(c, {.gap = 8, .justify = st::ui::Justify::End,
                               .key = "cfg-actions"}, [&] {
                   (void)button(c, "开始使用", [this] { save_settings(); },
                                {.variant = st::ui::Variant::Primary,
                                 .id = "cfg-save", .key = "cfg-save"});
                 });
               });
  }

  /// 一行设置字段：标签 + 输入框 +（可选）说明。
  ///
  /// `hint` 为空时**不声明那一行文本**：空占位符没信息量，还会把相邻字段
  /// 推远（屏幕上看着像“报错提示区永远是空的”）。
  ///
  /// `extra` 非空时在输入框**右侧**并排一个控件（如模型列表的下拉按钮）：
  /// 必须是“输入框 + 附件”横排，因为模型名既要**能手动输入**（自定义/私有模型），
  /// 又要**能从端点拉列表选**——两个需求共存只能并排，不能拆成两个区域。
  void settings_field(st::ui::dsl::Composer& c, const std::string& label, const std::string& hint,
                      const std::string& value, std::function<void(std::string)> on_input,
                      const std::string& field_id, bool password = false) {
    using namespace st::ui::dsl;
    (void)column(c, {.gap = 4, .key = field_id + "-wrap"}, [&] {
      (void)text(c, [label] { return label; }, {.size = 12.0f, .weight = st::ui::FontWeight::Bold,
                                                .key = field_id + "-label"});
      (void)input(c, value, std::move(on_input),
                  {.id = field_id, .key = field_id}, password);
      if (!hint.empty()) {
        (void)text(c, [hint] { return hint; }, {.color = st::ui::Tone::Muted, .size = 11.0f,
                                                .key = field_id + "-hint"});
      }
    });
  }

  // —— 模型选择：输入框**内嵌**下拉按钮 ——

  /// 模型字段的专用构造：`Input`（可手填）+ 框内右侧下拉按钮。
  ///
  /// 为何不用“Input + Select 并排”：那在视觉上是**两个控件**，而用户心里
  /// “模型”是一个字段。框架侧为此新增了 `Input::set_dropdown` /
  /// `set_dropdown_options`（箭头长在框内、命中区在框内、选中即回填）。
  ///
  /// 拉取时机：点箭头时若还没拉过列表，先拉一次（拉完自动能展开）。
  void model_field(st::ui::dsl::Composer& c) {
    using namespace st::ui::dsl;
    (void)column(c, {.gap = 4, .key = "cfg-model-wrap"}, [&] {
      (void)text(c, [] { return std::string("模型"); },
                 {.size = 12.0f, .weight = st::ui::FontWeight::Bold, .key = "cfg-model-label"});
      auto options = model_options.value();
      (void)custom<st::ui::Input>(
          c,
          [this, options](st::ui::Input& in) {
            in.set_id("cfg-model");
            in.set_placeholder("模型名，可手填");
            // 逐帧同步值（不 gegen 用户正在输入的那一帧：只在值真变了时才写）。
            if (in.value() != draft_model.value()) in.set_text(draft_model.value());
            in.on_change = [this](std::string_view text) { draft_model.set(std::string(text)); };
            in.set_dropdown(true);
            std::vector<st::ui::SelectOption> select_options;
            select_options.reserve(options.size());
            for (const auto& name : options) {
              st::ui::SelectOption option;
              option.value = name;
              option.label = name;
              select_options.push_back(std::move(option));
            }
            in.set_dropdown_options(std::move(select_options));
            // **选项还没拉过时点箭头也要有反应**：启动拉取，拉到后自动展开。
            //
            // 首次填配置就是这个情形：`open_settings` 里那份预拉取用的是
            // **已保存的** 配置（此时是空的），所以不会触发；用户随后手填
            // URL/Key，但没有任何时机再去拉。旧实现在这里直接“点了没反应”。
            in.on_dropdown_need_options = [this] {
              if (models_job != nullptr) return;   // 已在拉，等它回来就行
              start_model_fetch();
            };
            if (root_host != nullptr) {
              st::ui::UiRoot* host = root_host;
              in.overlay_host = [host](std::unique_ptr<st::ui::Element> panel) {
                host->add_overlay(std::move(panel));
              };
              in.overlay_remove = [host](st::ui::Element* panel) {
                host->remove_overlay(panel);
              };
            }
          },
          {.key = "cfg-model"});
      // 提示行：拉取中/失败信息，或“点右侧箭头拉取列表”。
      (void)text(c, [this] { return models_hint.value(); },
                 {.color = st::ui::Tone::Muted, .size = 11.0f, .key = "cfg-model-hint"});
    });
  }

  /// 拉取模型列表（后台线程）：`GET {base}/models`。
  void start_model_fetch() {
    if (models_job != nullptr) return;   // 已有在跑
    std::string base_url = draft_url.value();
    std::string api_key = draft_key.value();
    if (base_url.empty() || api_key.empty()) {
      models_hint.set("先填 Base URL 与 API Key");
      return;
    }
    models_hint.set("正在拉取…");
    auto next = std::make_unique<ModelListJob>();
    ModelListJob* raw = next.get();
    raw->worker = std::thread([raw, base_url = std::move(base_url),
                                api_key = std::move(api_key)] {
      std::string url;
      const std::string wire = build_models_request(base_url, api_key, url);
      const auto parsed = http::Url::parse(url);
      if (!parsed.has_value()) {
        const std::lock_guard<std::mutex> guard(raw->mutex);
        raw->error = "Base URL 非法：" + url;
        raw->finished.store(true);
        return;
      }
      if (parsed->scheme == "https") {
        auto tls = st::net::TlsStream::connect(parsed->host, parsed->port, 15000);
        if (!tls.has_value()) {
          const std::lock_guard<std::mutex> guard(raw->mutex);
          raw->error = "TLS 连接失败：" + tls.error().message;
          raw->finished.store(true);
          return;
        }
        fetch_model_list(*tls, *raw, wire);
      } else {
        auto tcp = st::net::connect_tcp(parsed->host, parsed->port, 15000);
        if (!tcp.has_value()) {
          const std::lock_guard<std::mutex> guard(raw->mutex);
          raw->error = "TCP 连接失败：" + tcp.error().message;
          raw->finished.store(true);
          return;
        }
        fetch_model_list(*tcp, *raw, wire);
      }
    });
    models_job = std::move(next);
  }

  /// 每帧收取拉取结果（主线程）。
  [[nodiscard]] auto pump_model_list() -> bool {
    if (models_job == nullptr) return false;
    std::vector<std::string> list;
    std::string error;
    if (!models_job->drain(list, error)) return false;
    if (models_job->worker.joinable()) models_job->worker.join();
    models_job.reset();
    if (!error.empty()) {
      models_hint.set(error);
      return true;
    }
    model_options.set(std::move(list));
    models_hint.set({});
    return true;
  }

  // —— 行为 ——
  void open_settings() {
    const Config cfg = config.value();
    draft_url.set(cfg.base_url);
    draft_key.set(cfg.api_key);
    draft_model.set(cfg.model);
    // 打开面板时顺手拉一次模型列表：用户点箭头就能直接选，
    // 不用先“点一下拉取、再点一下展开”两步。已有列表/正在拉则不动。
    if (model_options.value().empty() && models_job == nullptr &&
        !cfg.base_url.empty() && !cfg.api_key.empty()) {
      start_model_fetch();
    }
    settings_open.set(true);
  }

  void save_settings() {
    Config cfg = config.value();
    cfg.base_url = draft_url.value();
    cfg.api_key = draft_key.value();
    if (!model_locked()) cfg.model = draft_model.value();  // 预置的模型不被界面覆盖
    config.set(cfg);
    // **不落盘**：设置只在本进程内存里生效（关窗即忘）。
    settings_open.set(false);
    status.set(cfg.ready() ? std::string{} : "配置不完整（需要 base_url / api_key）");
  }

  // ————————————————— 工具声明与执行 —————————————————

  /// 工具的 OpenAI `tools` 声明。四个工具：sh / read / write / edit。
  ///
  /// ⚠ **不要用`{{"k", v}}`这样的嵌套花括号去初始化 `Json`**：
  /// nlohmann 会把它解成**数组**（`[{"k": v}]` 这种），而不是对象。
  /// 实测代价：构造出来的 `parameters` 不是对象，发出去后模型侧不认；
  /// 本地把 `index` 当数字读时还会抛 `type must be number, but is object`
  /// 直接打死进程。所以这里全程 `Json::object()` + 逐键赋值。
  [[nodiscard]] auto tool_declarations() -> Json {
    if (!tools_enabled.value()) return Json::array();
    const auto field = [](const std::string& type, const std::string& desc) {
      Json spec = Json::object();
      spec["type"] = type;
      spec["description"] = desc;
      return spec;
    };
    /// 组一个 `{type: object, properties: {...}, required: [...]}`。
    const auto schema = [](const std::vector<std::pair<std::string, Json>>& props,
                           const std::vector<std::string>& required) {
      Json properties = Json::object();
      for (const auto& [name, spec] : props) properties[name] = spec;
      Json req = Json::array();
      for (const auto& name : required) req.push_back(name);
      Json out = Json::object();
      out["type"] = "object";
      out["properties"] = std::move(properties);
      out["required"] = std::move(req);
      return out;
    };
    const auto declare = [](const std::string& name, const std::string& desc, Json parameters) {
      Json fn = Json::object();
      fn["name"] = name;
      fn["description"] = desc;
      fn["parameters"] = std::move(parameters);
      Json tool = Json::object();
      tool["type"] = "function";
      tool["function"] = std::move(fn);
      return tool;
    };

    Json tools = Json::array();
    tools.push_back(declare(
        "sh",
        "在本机执行一条 shell 命令（Windows 上是 PowerShell，POSIX 上是 bash），返回输出与退出码。"
        "用于查看目录、跑测试、执行构建等。",
        schema({{"command", field("string", "要执行的命令")}}, {"command"})));
    tools.push_back(declare(
        "read",
        "读取一个文本文件（输出带行号）。路径相对工作目录；绝对路径原样使用。",
        schema({{"path", field("string", "文件路径")},
                 {"offset", field("integer", "从第几行开始（1 起，可省）")},
                 {"limit", field("integer", "最多读多少行（可省）")}},
               {"path"})));
    tools.push_back(declare(
        "write", "写入（覆盖）一个文本文件，父目录不存在会自动创建。整个文件内容都要给出。",
        schema({{"path", field("string", "文件路径")},
                 {"content", field("string", "完整文件内容")}},
               {"path", "content"})));
    tools.push_back(declare(
        "edit",
        "把文件中的 old_string 全部替换为 new_string。old_string 必须在文件里存在"
        "（建议先用 read 确认原文）。",
        schema({{"path", field("string", "文件路径")},
                 {"old_string", field("string", "要被替换的原文（必须存在）")},
                 {"new_string", field("string", "替换后的内容")}},
               {"path", "old_string", "new_string"})));
    return tools;
  }

  /// 执行一次工具调用。**永远返回结果**（失败也是给模型的信息）。
  [[nodiscard]] auto run_tool(const ToolCall& call) -> ToolResult {
    const auto parsed = st::json_parse(call.arguments);
    if (!parsed.has_value()) {
      return ToolResult{false, "参数不是合法 JSON：" + call.arguments};
    }
    const Json& args = *parsed;

    if (call.name == "sh") {
      const std::string command = tool_arg(args, "command");
      if (command.empty()) return ToolResult{false, "command 不能为空"};
      st::process::Options options;
      options.cwd = tool_root;
      options.capture_output = true;
      options.merge_stderr = true;   // 顺序敏感的日志：stderr 混在 stdout 里才对得上
      options.timeout_ms = kToolShellTimeoutMs;
#if defined(_WIN32)
      auto ran = st::process::run("powershell", {"-NoProfile", "-NonInteractive",
                                                "-Command", command}, options);
#else
      auto ran = st::process::run("bash", {"-lc", command}, options);
#endif
      if (!ran.has_value()) {
        return ToolResult{false, "执行失败：" + ran.error().message};
      }
      std::string text = ran->stdout_text;
      if (text.empty()) text = "（无输出）";
      text += "\n[exit " + std::to_string(ran->exit_code) + "]";
      return ToolResult{ran->exit_code == 0, truncate_tool_output(std::move(text))};
    }

    if (call.name == "read") {
      const std::string path = resolve_tool_path(tool_root, tool_arg(args, "path"));
      auto loaded = st::fs::read_text(path);
      if (!loaded.has_value()) {
        return ToolResult{false, "读取失败：" + loaded.error().message + "（路径：" + path + "）"};
      }
      std::string content = std::move(*loaded);
      // 行号 + 可选切片：与常见编辑器/工具的输出形状一致，模型能直接引用行号。
      std::size_t offset = 1;
      std::size_t limit = 0;
      if (const Json* value = st::json_find(args, "offset");
          value != nullptr && value->is_number()) {
        offset = static_cast<std::size_t>(std::max<std::int64_t>(1, st::json_as_i64(*value)));
      }
      if (const Json* value = st::json_find(args, "limit");
          value != nullptr && value->is_number()) {
        limit = static_cast<std::size_t>(std::max<std::int64_t>(0, st::json_as_i64(*value)));
      }
      std::string out;
      std::size_t line_no = 0;
      std::size_t taken = 0;
      std::size_t at = 0;
      while (at <= content.size()) {
        const auto end = content.find('\n', at);
        const std::string_view line =
            end == std::string::npos ? std::string_view(content).substr(at)
                                     : std::string_view(content).substr(at, end - at);
        ++line_no;
        if (line_no >= offset && (limit == 0 || taken < limit)) {
          out += std::to_string(line_no);
          out += "\t";
          out.append(line);
          out += '\n';
          ++taken;
        }
        if (end == std::string::npos) break;
        at = end + 1;
      }
      if (out.empty()) out = "（空文件或范围超出）";
      return ToolResult{true, truncate_tool_output(std::move(out))};
    }

    if (call.name == "write") {
      const std::string path = resolve_tool_path(tool_root, tool_arg(args, "path"));
      const std::string content = tool_arg(args, "content");
      if (auto made = st::fs::ensure_parent(path); !made) {
        return ToolResult{false, "创建父目录失败：" + made.error().message};
      }
      if (auto wrote = st::fs::write_text(path, content); !wrote) {
        return ToolResult{false, "写入失败：" + wrote.error().message};
      }
      return ToolResult{true, "已写入 " + std::to_string(content.size()) + " 字节。" + path};
    }

    if (call.name == "edit") {
      const std::string path = resolve_tool_path(tool_root, tool_arg(args, "path"));
      return tool_edit_text(path, tool_arg(args, "old_string"), tool_arg(args, "new_string"));
    }

    return ToolResult{false, "未知工具：" + call.name};
  }

  /// 把界面消息投影成 OpenAI `messages` 数组（回传给模型的那份）。
  ///
  /// 三个要点：
  /// - **推理过程不回传**（`reasoning_content` 只在界面上看）；
  /// - 带 `tool_calls` 的助手消息必须原样回传（模型据此知道“我说过要调什么”）；
  /// - `tool` 消息必须带 `tool_call_id`（少了网关会 400）。
  [[nodiscard]] auto history_for_api() -> std::vector<Json> {
    std::vector<Json> out;
    std::size_t rounds = 0;
    for (const auto& m : messages.value()) {
      if (m.role == "tool") {
        Json entry = Json::object();
        entry["role"] = "tool";
        entry["tool_call_id"] = m.tool_call_id;
        entry["content"] = m.content;
        out.push_back(std::move(entry));
        continue;
      }
      if (m.role == "assistant") {
        // 完全空的助手消息（正在流式的那条）不进上下文。
        if (m.content.empty() && m.tool_calls.empty()) continue;
        Json entry = Json::object();
        entry["role"] = "assistant";
        entry["content"] = m.content;
        if (!m.tool_calls.empty()) {
          Json calls = Json::array();
          for (const auto& call : m.tool_calls) {
            Json fn = Json::object();
            fn["name"] = call.name;
            fn["arguments"] = call.arguments;
            Json item = Json::object();
            item["id"] = call.id;
            item["type"] = "function";
            item["function"] = std::move(fn);
            calls.push_back(std::move(item));
          }
          entry["tool_calls"] = std::move(calls);
        }
        out.push_back(std::move(entry));
        continue;
      }
      if (m.content.empty()) continue;
      Json entry = Json::object();
      entry["role"] = m.role;
      entry["content"] = m.content;
      out.push_back(std::move(entry));
      (void)rounds;
    }
    return out;
  }

  /// 发起一次模型请求（后台线程）。**多轮循环的单一入口**：
  /// 首轮、工具执行后的续轮都走这里。
  void start_request() {
    const Config cfg = config.value();
    auto next = std::make_unique<StreamJob>();
    StreamJob* raw = next.get();
    Config cfg_copy = cfg;
    Json tools = tool_declarations();
    std::vector<Json> context = history_for_api();
    raw->worker = std::thread([raw, cfg_copy, tools = std::move(tools),
                                context = std::move(context)] {
      run_request(raw, cfg_copy, tools, context);
    });
    job = std::move(next);
  }

  void send() {
    const std::string text = trim(draft.value());
    if (text.empty() || busy.value()) return;
    const Config cfg = config.value();
    if (!cfg.ready()) {
      if (!settings_open.value()) open_settings();
      return;
    }

    append_message(Message{.role = "user", .content = text, .stamp = now_stamp()});
    append_message(Message{.role = "assistant", .content = "", .stamp = now_stamp()});
    draft.set({});
    stream_text.clear();
    stream_reasoning.clear();
    pending_calls.clear();
    tool_rounds = 0;
    status.set("正在思考…");
    busy.set(true);
    start_request();
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

    std::vector<StreamDelta> deltas;
    std::string error;
    job->drain(deltas, error);
    if (!deltas.empty()) {
      bool content_changed = false;
      bool reasoning_changed = false;
      for (auto& delta : deltas) {
        if (!delta.content.empty()) {
          stream_text += delta.content;
          content_changed = true;
        }
        if (!delta.reasoning.empty()) {
          stream_reasoning += delta.reasoning;
          reasoning_changed = true;
        }
        for (auto& chunk : delta.tools) {
          // 同一 `index` 的分片要拼到同一条上（首片带 id/name，后续只带参数）。
          while (pending_calls.size() <= chunk.index) pending_calls.push_back(ToolCall{});
          ToolCall& slot = pending_calls[chunk.index];
          if (!chunk.id.empty()) slot.id = chunk.id;
          if (!chunk.name.empty()) slot.name = chunk.name;
          slot.arguments += chunk.args;
          touched = true;
        }
      }
      if (content_changed || reasoning_changed) {
        auto list = messages.value();
        if (auto updated = last_assistant_index(&list); updated.has_value()) {
          if (content_changed) list[*updated].content = stream_text;
          if (reasoning_changed) {
            list[*updated].reasoning = stream_reasoning;
            list[*updated].reasoning_streaming = true;
          }
          messages.set(std::move(list));
        }
        if (content_changed) {
          status.set("生成中…（" + std::to_string(stream_text.size()) + " 字符）");
        } else if (reasoning_changed) {
          status.set("思考中…");
        }
        touched = true;
      }
    }

    if (!job->finished.load()) return touched;

    // —— 本轮结束：收尾并决定要不要进入下一轮（工具调用）——
    if (job->worker.joinable()) job->worker.join();
    job.reset();

    auto list = messages.value();
    const auto index = last_assistant_index(&list);
    if (!error.empty() && error != "cancelled") {
      if (index.has_value()) {
        list[*index].content = stream_text.empty() ? "**请求失败**：" + error : stream_text;
        list[*index].reasoning_streaming = false;
        messages.set(std::move(list));
      }
      status.set("请求失败：" + error);
      stream_text.clear();
      stream_reasoning.clear();
      pending_calls.clear();
      busy.set(false);
      return true;
    }
    if (error == "cancelled") {
      status.set("已停止");
      stream_text.clear();
      stream_reasoning.clear();
      pending_calls.clear();
      busy.set(false);
      return true;
    }

    // 把本轮的工具调用落到助手消息上（回传上下文时要原样带上）。
    const bool has_calls = !pending_calls.empty();
    if (index.has_value()) {
      list[*index].tool_calls = pending_calls;
      list[*index].reasoning_streaming = false;
      messages.set(std::move(list));
    }

    if (!has_calls) {
      // 没有工具调用 = 这一轮就是最终答复。
      status.set({});
      stream_text.clear();
      stream_reasoning.clear();
      busy.set(false);
      return true;
    }

    // 有工具调用：执行 → 回传结果 → 再请求（多轮交互的核心）。
    if (++tool_rounds > kMaxToolRounds) {
      append_message(Message{.role = "assistant",
                             .content = "工具调用轮数已达上限（" +
                                        std::to_string(kMaxToolRounds) + "），已停下来。",
                             .stamp = now_stamp()});
      status.set({});
      stream_text.clear();
      stream_reasoning.clear();
      pending_calls.clear();
      busy.set(false);
      return true;
    }

    const std::vector<ToolCall> calls = pending_calls;
    pending_calls.clear();
    stream_text.clear();
    stream_reasoning.clear();

    // **同步执行**工具：工具本身很快（read/write/edit 是毫秒级，sh 有 60s 起限），
    // 而产出必须紧接着回传才能进入下一轮。界面在下一帧才重组，所以这里不卡帧。
    for (const auto& call : calls) {
      status.set("执行工具：" + call.name + "…");
      ToolResult result = run_tool(call);
      Message tool_msg{.role = "tool", .content = result.text, .stamp = now_stamp()};
      tool_msg.tool_call_id = call.id;
          tool_msg.tool_name = call.name;
    tool_msg.tool_arguments = call.arguments;
    tool_msg.output = result.text;
      tool_msg.tool_failed = !result.ok;
      append_message(std::move(tool_msg));
    }

    // 为下一轮留一个空助手气泡（流式往它里面写）。
    append_message(Message{.role = "assistant", .content = "", .stamp = now_stamp()});
    status.set("正在思考…");
    start_request();
    return true;
  }

  /// 把当前模型名同步到标题栏按钮。
  ///
  /// 为何手动推：那个按钮是 `TitleBar::add_leading` 挂的壳层元素，不在本页的
  /// 声明树里，重组不会碰它。每帧写一次不贵（`set_label` 有相等早退）。
  void sync_title_badge() {
    if (model_badge == nullptr) return;
    if (auto* button = dynamic_cast<st::ui::Button*>(model_badge); button != nullptr) {
      const std::string wanted = config.value().model;
      if (button->label() != wanted) button->set_label(wanted);
    }
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
  static void run_request(StreamJob* raw, const Config& cfg, const Json& tools,
                          const std::vector<Json>& context) {
    std::string url;
    const std::string wire = build_request(cfg, context, tools, url);
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
      pump_stream(*tls, *raw, wire);
    } else {
      auto tcp = st::net::connect_tcp(parsed->host, parsed->port, 15000);
      if (!tcp.has_value()) {
        raw->fail("TCP 连接失败：" + tcp.error().message);
        raw->finished.store(true);
        return;
      }
      pump_stream(*tcp, *raw, wire);
    }
    raw->finished.store(true);
  }

  // —— 小工具 ——
  [[nodiscard]] static auto trim(std::string text) -> std::string {
    const auto not_space = [](unsigned char ch) { return ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t'; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
  }

  /// 消息时间戳：**北京时间（UTC+8）**，`HH:MM`。
  ///
  /// 为何不直接用本地时区：部署环境可能是任何时区（CI、容器里常是 UTC），
  /// 而用户要看的是北京时间——时区固定成常量才与运行环境无关。
  /// 折算本身在 `st::time::hhmm_at_offset`（与应用外共用同一入口，便于测试）。
  ///
  /// 为何用 `unix_ms()` 而不是 `now_ms()`：后者是**单调时钟**（进程内相对基准，
  /// 从 0 附近起算），拿它除 3600 得到的“小时”与真实时刻无关
  /// （实测：屏幕上恒显示 04:41，恰好是进程启动至今的小时数）。
  [[nodiscard]] static auto now_stamp() -> std::string {
    constexpr std::int64_t kBeijingOffsetSeconds = 8 * 3600;
    return st::time::hhmm_at_offset(st::time::unix_ms(), kBeijingOffsetSeconds);
  }
};

// ———————————————————————————————————————————————————————————————
// 入口
// ———————————————————————————————————————————————————————————————
int run_app(int argc, char** argv) {
  st::app::CommonOptions common;
  // **`--demo`：注入一组构造好的假消息**（不调模型）。
  //
  // 为何需要它：验证气泡/推理块的**形态**（边框、圆角、半透明）时，
  // 每次等模型真实流式返回要十几秒，而且内容长短不可控——
  // 观感类改动需要的是“固定内容 + 固定布局”的可重复画面。
  // 只在开发/截图时用，不影响正常对话路径。
  const std::vector<st::app::AppOption> app_options = {
      st::app::flag("--demo", "注入构造好的演示消息（不调模型；截图/调观感用）")};
  st::app::AppOptionValues values;
  auto parsed = st::app::parse_options(argc, argv, app_options, common, values);
  if (!parsed) {
    std::fprintf(stderr, "参数解析失败：%s\n", parsed.error().message.c_str());
    return 1;
  }
  if (common.show_help) {
    std::printf("小白（xiaobai）——极简 AI 智能体\n用法：xiaobai [通用选项]\n");
    return 0;
  }

  st::app::AppOptions options = common.app;
  if (options.width <= 0) options.width = 720;
  if (options.height <= 0) options.height = 560;
  if (options.title.empty()) options.title = "小白 · 霜天";
  // —— 窗口/任务栏图标 ——
  //
  // 这里和 `st.pkg` 的 `"icon": "*.ico"` **不是同一件事**：
  // `.ico` 进的是 PE 资源（资源管理器/快捷方式看到的），而**任务栏用的是窗口的
  // `WM_SETICON`**，只能由运行期位图给。只配 `.ico` 不设 `window_icon_png`，
  // 任务栏就是个空白默认图标（本应用之前就是这个状态）。
  st::app::icon::attach_png(options.window_icon_png,
                            b::embed<"apps/xiaobai/assets/xiaobai-icon.png">());

  st::app::Application app("xiaobai", "0.1.0", options);

  auto page = std::make_shared<AgentPage>();
  // 窗口标题：标题栏上已经显示 icon + 模型按钮，窗口标题保持为空，
  // 不会与品牌/模型名重复两个字（系统任务栏的标题仍由 `AppOptions::title` 给）。
  auto frame = std::make_unique<st::ui::WindowFrame>("");
  frame->set_id("window-frame");
  frame->set_window_control(&app);
  if (frame->title_bar() != nullptr) {
    frame->title_bar()->set_id("titlebar");
    // 品牌标记：火箭（SVG，与任务栏的 PNG 同一记号）。
    // 必须在首帧前注册——注册表是显式装载的，`TitleBar` 只在重绘时取图形。
    st::app::icon::register_svg("xiaobai",
                                b::embed<"apps/xiaobai/assets/xiaobai-logo.svg">());
    st::app::icon::apply_brand(frame->title_bar(), "xiaobai");
    // 标题栏从左到右：**[火箭图标][小白][模型]**。
    // “小白”走品牌名（紧跟在图标后），模型名跟在它后面（`leading` 槽）：
    // 两者都是 `TitleBar` 的既有位置语义，不另造容器。
    frame->title_bar()->set_brand("小白");

    // 模型名**可点击**：它同时是“当前模型”与“配置入口”，点一下开配置面板。
    // 图标只留文字（不要齿轮：那是“设置”的通用记号，而这里表达的是“当前模型”）。
    auto badge = std::make_unique<st::ui::Button>(page->config.value().model);
    badge->set_id("model-label");
    badge->set_key("model-label");
    badge->set_label(page->config.value().model);
    badge->set_variant(st::ui::Button::Variant::Ghost);
    badge->set_size(st::ui::Button::Size::Small);
    badge->set_hint("点击配置模型连接");
    badge->on_click = [page] { page->open_settings(); };
    auto* badge_raw = frame->title_bar()->add_leading(std::move(badge));
    page->model_badge = badge_raw;
  }
  st::ui::Element* slot = frame->content();
  app.set_content(std::move(frame));

  auto host = st::ui::dsl::mount_into(app.root(), *slot, page);
  if (host == nullptr) {
    std::fprintf(stderr, "声明式挂载失败\n");
    return 1;
  }
  // 下拉面板需要真值树根（挂叠加层）：在首帧前注入。
  page->root_host = &app.root();
  // 工具的工作目录 = 进程当前目录（模型给的相对路径都相对它）。
  // 用标准库 `std::filesystem` 取：它不是平台 API（跨平台同一行），
  // 而 `st::fs` 没暴露 cwd。
  {
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) page->tool_root = cwd.string();
  }
  (void)host->tick();

  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败：%s\n", status.error().message.c_str());
    return 1;
  }

  // **`--demo`：注入一组构造好的假消息**（不调模型）。
  //
  // 为何需要它：验证气泡/推理块的**形态**（边框、圆角、半透明）时，
  // 每次等模型真实流式返回要十几秒，而且内容长短不可控——
  // 观感类改动需要的是“固定内容 + 固定布局”的可重复画面。
  // 这个开关只在开发/截图时用，不影响正常对话路径。
  if (values.has("--demo")) {
    const std::string trace =
        "用户要了解液态玻璃。\n"
        "先想清楚它是什么：Apple 在 2025 年提出的设计语言，核心是折射而不是模糊。\n"
        "需要区分三个层次：材质（玻璃）、行为（折射/高光）、动态（随背景变化）。\n"
        "回答应该给一个最小可跑的实现，而不是只讲概念。";
    const std::string body =
        "## 液态玻璃的几个关键点\n\n"
        "**材質**：背景内容透过，但被模糊、加饱和，不是简单叠一层灰。\n\n"
        "**边缘折射**：玻璃边缘会把背景“扭曲”一下，像凸透镜。\n\n"
        "**高光反射**：边缘有一圈高光，会随倾斜角度/光标位置移动。\n\n"
        "```css\n.glass {\n  backdrop-filter: blur(20px) saturate(180%);\n"
        "  box-shadow: inset 0 1px 0 rgba(255,255,255,.6);\n}\n```";
    std::vector<Message> seeded;
    {
      Message user;
      user.role = "user";
      user.content = "简单介绍液态玻璃特效";
      user.stamp = "14:22";
      seeded.push_back(user);
    }
    {
      Message assistant;
      assistant.role = "assistant";
      assistant.reasoning = trace;
      assistant.reasoning_streaming = false;
      assistant.content = body;
      assistant.stamp = "14:22";
      seeded.push_back(assistant);
    }
    page->messages.set(seeded);
    page->status.set({});
  }

  // 首次运行引导：没有可用配置就把配置面板直接弹出来（无顶栏入口，这一步不可绕过）。
  // 必须在 start() 之后：弹窗走声明式 overlay，需要 UiRoot 与脚本宿主就绪。
  //
  // **demo 模式不弹**：它的用途是截图看形态，弹窗的遮罩会把整屏压暗，
  // 看到的颜色全不是真实主题（实测：遮罩下背景从 235 掉到 ~200，
  // 会让人误以为玻璃/背景本身变暗了）。
  const Config cfg = page->config.value();
  if (values.has("--demo") || cfg.ready()) {
    page->status.set({});
  } else {
    // 未配置时直接弹面板，不另写状态行（界面上不保留状态文字）。
    page->open_settings();
  }

  while (!app.quit_requested()) {
    // 流式增量必须先于重组推进：State 在此标脏，紧接着 tick 才能把改动落地。
    // 为何不放在 build() 里：build 只在重组时跑，而重组又靠“状态脏了”触发——
    // 谁先动的问题会变成静默死锁（实测：状态永远停在「正在连接…」）。
    if (page->pump_stream_into_state()) app.request_repaint();
    // 模型列表拉取结果（同样必须先于重组落地）。
    if (page->pump_model_list()) app.request_repaint();
    // 标题栏模型标签同步（壳层元素，重组不碰它）+ 尺寸/换行变化后重排。
    page->sync_title_badge();
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