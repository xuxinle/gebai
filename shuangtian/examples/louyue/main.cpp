/// 镂月（Lunaris）— 图像编辑器示例。
///
/// ## 这个示例检验什么（设计文档 §1）
///
/// **像素 / 光栅创作面**——gallery 是组件能力面、gbcode 是文本编辑器应用面，
/// 而镂月是唯一以**自绘画布**为主表面的示例。它把光栅管线拉到真实用户路径上：
/// `Canvas`/`Paint`/`BlendMode`/`Surface` 合成、逐像素操作、PNG 编解码**往返**、
/// 撤销栈、九种混合模式的肉眼与像素双重检验。
///
/// ## 壳是声明式的，画布走逃生舱
///
/// 菜单/工具栏/图层面板/状态栏都用 `dsl` 描述；画布这类高频自绘走
/// `dsl::custom<T>`（与 gbcode 持有 `CodeEditor` 同构）。见设计文档 §9。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "canvas_view.hpp"
#include "document.hpp"
#include "assets_lib.hpp"
#include "poster.hpp"
#include "poster_bridge.hpp"
#include "battery/embed.hpp"   // 编译期资源嵌入（stpm 生成）
#include "st/app/app.hpp"
#include "st/codec/png.hpp"
#include "st/math/geometry.hpp"
#include "st/core/entry.hpp"   // `ST_MAIN`（跨平台入口：正规化 argv 编码）
#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/core/log.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/color_picker.hpp"
#include "st/ui/components/file_dialog.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/slider.hpp"
#include "st/ui/components/split_view.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"
#include "st/ui/dsl.hpp"

namespace {

using louyue::CanvasView;
using louyue::CanvasViewport;
using louyue::Document;
using louyue::Swatch;
using louyue::Tool;
using st::math::Color;
using st::ui::Button;
namespace dsl = st::ui::dsl;

/// 命令行选项（承 gallery/gbcode 的形态：只解析本示例关心的几个）。
struct Options {
  bool headless{false};
  std::string backend{};
  std::string theme{"light"};
  std::uint16_t control_port{0};
  std::string control_file{};
  std::string shots{};
  std::string open{};   ///< `--open <png>`：启动即打开一个真实 PNG 文件
};

/// 用法说明。**必须有**：未知参数会报错退出，没有 `--help` 就等于"报错了也没处查"
///（承 gbcode 同名函数）。
auto print_usage(std::string_view program) -> void {
  st::log::out(
      "用法: {} [选项]\n"
      "\n"
      "窗口/主题\n"
      "  --headless            无头模式（无窗口；控制通道照常可用）\n"
      "  --theme light|dark    主题\n"
      "\n"
      "内容/自动化\n"
      "  --open PNG            启动即打开一个真实 PNG 文件\n"
      "  --control-port N      控制通道端口（0 = 自动）\n"
      "  --control-file PATH   控制信息落盘路径\n"
      "  --shots DIR           无头截图目录\n"
      "  -h, --help            显示本帮助\n",
      program);
}

auto parse_options(int argc, char** argv) -> Options {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string raw = argv[i];
    const auto value = [&](const char* fallback) -> std::string {
      if (i + 1 < argc) return argv[++i];
      return fallback;
    };
    // —— 数值参数统一走带校验的解析（承 gbcode 的同条纪律，逐字对齐）——
    //
    // 为何不能裸调 `std::stoi`：传非数字（如 `--control-port abc`）时它抛
    // `std::invalid_argument`，无人捕获 → `terminate`（实测退出码 0xC0000409，
    // 屏幕上一行 what(): stoi）——用户看不懂、脚本拿不到码。而同一命令行里的
    // `--bogus-flag` 走的是「报错 + exit 2」的干净路径。**非法值必须与未知参数
    // 同一条路径**（报错可读、退出码稳定），不能一快一崩。
    const auto parse_int = [&](const char* flag, std::int64_t fallback) -> std::int64_t {
      const std::string text = value(std::format("{}", fallback).c_str());
      std::int64_t parsed = 0;
      std::size_t consumed = 0;
      const bool ok = !text.empty() && [&] {
        try {
          parsed = std::stoll(text, &consumed);
        } catch (const std::exception&) {
          return false;
        }
        return consumed == text.size();   // 尾巴（如 `8x`）也是非法值
      }();
      if (!ok) {
        st::log::err("参数 {} 需要一个整数，得到：{}（用 --help 看可用项）\n", flag, text);
        std::exit(2);
      }
      return parsed;
    };
    if (raw == "--help" || raw == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    }
    if (raw == "--headless") {
      options.headless = true;
    }
    // ⚠ 独立 if 与下方 else-if 链的陷阱：命中独立 if 的参数若继续落进链尾
    // else 兜底会被误判「未知参数」——所以 headless 必须与链同体（首个 else-if）。
    else if (raw == "--backend") options.backend = value("");
    else if (raw == "--theme") options.theme = value("light");
    else if (raw == "--control-port") {
      const std::int64_t port = parse_int("--control-port", 0);
      if (port < 0 || port > 65535) {
        st::log::err("参数 --control-port 超出端口范围：{}\n", port);
        std::exit(2);
      }
      options.control_port = static_cast<std::uint16_t>(port);
    }
    else if (raw == "--control-file") options.control_file = value("");
    else if (raw == "--shots") options.shots = value("");
    else if (raw == "--open") options.open = value("");
    // **未知参数报错，不静默忽略**（gbcode 实证过的温床：新加的开关被静默
    // 吃掉，表现是"三档渲染出来的图几乎一样"，差点被当成结论）。
    else {
      st::log::err("未知参数：{}（用 --help 看可用项）\n", raw);
      std::exit(2);
    }
  }
  return options;
}

/// 主页面：整个编辑器是一个 `Component`。
///
/// ⚠ 生命周期契约（`docs/declarative.md` §5.5）：`build()` 会被**反复调用**
/// （任何 State 失效都触发重组），所以里面只做「声明」；`document_` 与
/// `canvas_` 是**跨帧存活**的真值源，不进声明式树。
class LouyuePage final : public dsl::Component {
 public:
  LouyuePage() : document_(std::make_unique<Document>(320, 200)) {
    // 启动即载入内嵌样例：PNG **解码**第一次跑在真实用户路径上（设计文档 §3.3）。
    //
    // 资源经 `st.pkg` 的 `embed` 声明、由 stpm 在编译期烤进二进制
    //（`battery/embed.hpp`）——内嵌路径相对**工程根**，与 gbcode/gallery 同口径。
    //
    // ⚠ 嵌入字节是 `char` 数组（`std::span<const char>`），而 `png_decode` 收
    // `std::span<const std::uint8_t>`——用 `std::transform` 拷一份，
    // **不用 `reinterpret_cast`**（lint L6 禁止；且 char→uint8_t 的别名转换
    // 在“char 为有符号”的平台上语义上就不干净）。样例图只有几 KB，开销可忽略。
    const auto raw = b::embed<"examples/louyue/assets/sample.png">();
    // ⚠ `EmbeddedFile` 不是容器：用 `data()` / `length()`（与 gallery 同口径）。
    const std::string_view raw_view(raw.data(), raw.length());
    st::codec::Bytes png;
    png.resize(raw_view.size());
    std::transform(raw_view.begin(), raw_view.end(), png.begin(), [](char raw_byte) {
      return static_cast<std::uint8_t>(static_cast<unsigned char>(raw_byte));
    });
    if (const auto image = st::codec::png_decode(png); image.has_value()) {
      (void)document_->load_png(*image);
      document_->set_file_path("sample.png");
    }
  }

  [[nodiscard]] auto document() noexcept -> Document& { return *document_; }
  [[nodiscard]] auto canvas() noexcept -> CanvasView* { return canvas_; }
  /// 菜单「退出」（主循环退出位；窗口关闭钮走 `app.quit_requested()`）。
  [[nodiscard]] auto quit_requested() const noexcept -> bool { return quit_requested_; }

  /// 文字端口（海报渲染需要字体；`Application::root().text_port()` 给）。
  ///
  /// 这里顺手渲染默认海报：字体在 `start()` 之后才可用，没有这一步的后果不是报错，
  /// 而是**海报里一个字都没有**（文字层静默跳过）。
  void set_text_port(const st::ui::TextPort* port) {
    text_port_ = port;
    apply_poster(poster_);
    // 首帧已过（`apply_poster` 里的 `fit_pending_` 在首帧被消费掉了），所以这里
    // 直接按当前视口尺寸算一次适配缩放。**不依赖 `bounds()`**：视口的 `fit_to_view`
    // 需要几何，而这里只想要“装得下”——视口宽从 `width()` 拿不到时就不适配，
    // 等用户自己滚滚轮（不报错）。
    if (canvas_ != nullptr) {
      if (auto* shell = dynamic_cast<CanvasViewport*>(canvas_->viewport()); shell != nullptr) {
        const float view_w = shell->bounds().width;
        const float view_h = shell->bounds().height;
        const float doc_w = static_cast<float>(document_->width());
        const float doc_h = static_cast<float>(document_->height());
        if (view_w > 8.0f && view_h > 8.0f && doc_w > 0.0f && doc_h > 0.0f) {
          const float fit = std::min(view_w / doc_w, view_h / doc_h);
          if (fit > 0.01f && fit < 0.999f) {
            shell->fit_to_view();
            canvas_version_.set(canvas_version_.value() + 1U);
          }
        }
      }
    }
  }

  /// 切换控制权的**唯一实现**（人拨开关 / AI 发 `edit_lock` / 启动参数都走它）。
  ///
  /// 为何收成一处：切控制权不只是改一个 bool——还要重画画布、刷新面板、落日志。
  /// 这些步骤散在多个入口里，新加一个入口就会漏一步，而漏的那步表现为
  /// “切了但界面没跟上”。（裁云同构，见 `CaiyunPage::set_control`。）
  void set_control(bool on, std::string_view source) {
    if (!document_->set_ai_control(on)) return;
    invalidate_all();
    log(on ? std::format("AI 接管（{}）：人工控件已置灰，仍可查看", source)
           : std::format("人拿回主导（{}）：人工控件已恢复", source));
  }

  /// 应用一份海报脚本：解析 → 渲染成图层 → 换掉当前文档。
  ///
  /// 为何**换文档**而不是往现有文档里画：脚本是作品真值，一个人工的旧文档混在里面
  /// 会让“这份海报到底是什么”变成两套说法。要保留手绘内容，另存 PNG 再合成。
  auto apply_poster_text(const std::string& json) -> st::Result<std::string> {
    const auto parsed = louyue::parse_poster(json);
    if (!parsed) return st::forward_error(parsed.error());
    louyue::PosterScript poster = parsed.value();
    if (!poster_script_path_.empty()) rebase_poster_paths(poster);
    apply_poster(poster);
    return poster.name;
  }

  /// 从文件装载海报脚本。失败时**保留原作品**并返回可读原因。
  auto load_poster(const std::string& path) -> st::Result<std::string> {
    const auto text = st::fs::read_text(path);
    if (!text) return st::forward_error(text.error());
    const auto parsed = louyue::parse_poster(text.value());
    if (!parsed) return st::forward_error(parsed.error());
    louyue::PosterScript poster = parsed.value();
    poster_script_path_ = path;
    rebase_poster_paths(poster);
    apply_poster(poster);
    return poster.name;
  }

  [[nodiscard]] auto save_poster(const std::string& path) const -> bool {
    return st::fs::write_text(path, louyue::poster_to_json(poster_)).has_value();
  }

  /// 导出当前文档为 PNG（`spec` = `path` 或 `path WxH`）。
  ///
  /// 尺寸不同时**按脚本重新渲染**（而不是缩放像素）：海报的层次是矢量摆放关系，
  /// 缩放会把字号/间距一起拉糊；重渲染在那个尺寸下重新算排版。
  auto export_poster(const std::string& spec) -> std::string {
    std::string path = spec;
    int width = 0;
    int height = 0;
    const std::size_t space = spec.rfind(' ');
    if (space != std::string::npos) {
      const std::string tail = spec.substr(space + 1);
      const std::size_t x = tail.find('x');
      if (x != std::string::npos) {
        try {
          width = std::stoi(tail.substr(0, x));
          height = std::stoi(tail.substr(x + 1));
          path = spec.substr(0, space);
        } catch (...) {
          width = 0;
          height = 0;
        }
      }
    }
    // 没给尺寸就按**作品设计尺寸**导出（而不是当前画布尺寸）。
    //
    // 画布会被 `sync_poster_to_viewport` 缩成视口大小（860×483），那是**看**的尺寸；
    // 交付物要的是脚本里的 1920×1080。两者混用会导出小图（实测过）。
    if (width <= 0 || height <= 0) {
      width = poster_.width;
      height = poster_.height;
    }
    if (path.empty()) return "export_png 需要输出路径（如 /tmp/poster.png）";
    if (width > 0 && height > 0) {
      louyue::PosterScript sized = poster_;
      sized.out_width = std::clamp(width, 64, 7680);
      sized.out_height = std::clamp(height, 64, 7680);
      st::raster::Canvas canvas{sized.export_width(), sized.export_height(), 1.0f};
      louyue::render_poster(canvas, sized, text_port_, &assets_, asset_basis_);
      st::codec::PngImage image;
      image.width = static_cast<std::uint32_t>(canvas.physical_width());
      image.height = static_cast<std::uint32_t>(canvas.physical_height());
      image.rgba = canvas.to_rgba8();
      if (!st::codec::png_write_file(path, image, 3).has_value()) {
        return std::format("写文件失败：{}", path);
      }
      last_export_ = path;
      return {};
    }
    return std::format("导出尺寸无效：{}x{}", width, height);
  }

  /// 载入一个真实 PNG 文件（`--open` 与菜单「文件→打开」共用）。
  auto open_file(const std::string& path) -> bool {
    const auto image = st::codec::png_read_file(path);
    if (!image.has_value()) return false;
    if (!document_->load_png(*image)) return false;
    document_->set_file_path(path);
    canvas_version_.set(canvas_version_.value() + 1U);
    return true;
  }

  /// 素材库根：与裁云共用生成的品牌素材（同一批图，两份用途）。
  void set_asset_root(std::string root) {
    assets_.set_root(std::move(root));
    (void)assets_.refresh();
    asset_basis_ = assets_.root();
  }

  void build(dsl::Composer& c) override {
    (void)layers_version_.value();
    (void)canvas_version_.value();
    dsl::column(c, {.gap = 0.0f, .grow = true, .id = "louyue-root"}, [&] {
      // 控制面（0×0）：AI 经 `invoke apply_poster` / `export_png` / `edit_lock`
      // 驱动本应用的唯一稳定寻址点。放在根列**第一个**，任何布局下都存在。
      (void)dsl::custom<louyue::PosterBridge>(
          c,
          [this](louyue::PosterBridge& bridge) {
            bridge.set_id("poster");
            bridge.set_hooks(make_poster_hooks());
          },
          {.width = 0.0f, .height = 0.0f, .key = "poster-bridge"});
      build_menu_bar(c);
      dsl::row(c, {.gap = 0.0f, .grow = true, .id = "main-row"}, [&] {
        build_toolbar(c);
        build_viewport(c);
        build_side_panels(c);
      });
      build_status_bar(c);
    });
    build_file_dialog(c);
  }

  // —— 供 e2e 的动作面（设计文档 §3.4，**不可改名**）——

  /// 海报控制面的钩子（全部转发到本页真实能力；桥里不做业务判断）。
  [[nodiscard]] auto make_poster_hooks() -> louyue::PosterBridge::Hooks {
    louyue::PosterBridge::Hooks hooks{};
    hooks.apply_poster = [this](const std::string& json) -> std::string {
      const auto result = apply_poster_text(json);
      return result ? std::string{} : result.error().message;
    };
    hooks.load_poster = [this](const std::string& path) -> std::string {
      const auto result = load_poster(path);
      return result ? std::string{} : result.error().message;
    };
    hooks.save_poster = [this](const std::string& path) -> std::string {
      if (path.empty()) return std::string("save_poster 需要文件路径");
      return save_poster(path) ? std::string{} : std::format("写入失败：{}", path);
    };
    hooks.export_png = [this](const std::string& spec) -> std::string {
      return export_poster(spec);
    };
    hooks.poster_json = [this] { return louyue::poster_to_json(poster_); };
    hooks.poster_value = [this](const std::string& key) -> std::string {
      if (key == "blocks") return std::to_string(poster_.blocks.size());
      if (key == "layer_count") return std::to_string(document_->layers().size());
      if (key == "size") return std::format("{}x{}", poster_.width, poster_.height);
      if (key == "export_size") {
        return std::format("{}x{}", poster_.export_width(), poster_.export_height());
      }
      if (key == "name") return poster_.name;
      return {};
    };
    hooks.set_edit_lock = [this](bool on) -> std::string {
      set_control(on, "AI 请求");
      return {};
    };
    hooks.edit_locked = [this] { return document_->ai_controlled; };
    hooks.last_export = [this] { return last_export_; };
    return hooks;
  }

  auto invoke_export(const std::string& path) -> bool { return document_->export_png(path); }
  auto invoke_undo() -> bool { return document_->undo(); }
  auto invoke_redo() -> bool { return document_->redo(); }
  auto add_layer() -> bool {
    if (document_->layers().size() >= 16U) return false;   // 上限护栏：图层面板放不下更多
    document_->add_layer(std::format("图层 {}", document_->layers().size() + 1U));
    invalidate_all();
    return true;
  }
  auto remove_layer(std::size_t index) -> bool {
    const bool removed = document_->remove_layer(index);
    if (removed) invalidate_all();
    return removed;
  }
  auto set_blend(std::size_t index, int mode) -> bool {
    if (index >= document_->layers().size()) return false;
    if (mode < 0 || mode >= static_cast<int>(louyue::kBlendModeCount)) return false;
    document_->layers()[index].blend = louyue::kBlendModes[mode];
    invalidate_all();
    return true;
  }
  [[nodiscard]] auto layer_summary() const -> std::string {
    std::string out;
    for (const auto& layer : document_->layers()) {
      out += std::format("{}|{}|{:.2f}|{}\n", layer.name,
                         st::raster::to_string(layer.blend), static_cast<double>(layer.opacity),
                         layer.visible ? "1" : "0");
    }
    return out;
  }
  [[nodiscard]] auto history_summary() const -> std::string {
    std::string out;
    for (const auto& name : document_->history_names()) out += name + "\n";
    return out;
  }

 private:
  void invalidate_all() {
    canvas_version_.set(canvas_version_.value() + 1U);
    layers_version_.set(layers_version_.value() + 1U);
    if (canvas_ != nullptr) canvas_->mark_dirty();
  }

  /// 菜单栏（与 gbcode 同一条 `dsl::menu_bar` + `menu_panel_overlay` 链路）。
  ///
  /// 「文件→打开/另存」曾是注释里的虚指（`--open` 注释声称共用，菜单却
  /// 不存在）——这不是补一个装饰：`open_file`/`export_png` 的能力早就在，
  /// 缺的只是 UI 入口。`FileDialog` 是框架组件（目录浏览/双击进入/面包屑
  /// 都现成），无头下还可程序化选路（`set_pending_path`）。
  void build_menu_bar(dsl::Composer& c) {
    const std::vector<dsl::MenuData> menus = {
        {"file", "文件",
         {{.id = "open", .label = "打开 PNG…"},
          {.id = "save-as", .label = "导出 PNG…"},
          {.separator = true},
          {.id = "quit", .label = "退出"}}},
    };
    menu_bar_ = dsl::menu_bar(
        c, menus,
        [this](const std::string& menu, const std::string& item) {
          if (menu != "file") return;
          if (item == "open") {
            file_dialog_mode_ = 0;
            file_dialog_open_.set(true);
          } else if (item == "save-as") {
            file_dialog_mode_ = 1;
            file_dialog_open_.set(true);
          } else if (item == "quit") {
            quit_requested_ = true;
          }
        },
        [this](std::size_t index) {
          // 与 gbcode 的 MenuHeader 同构：再点同一标题 = 收起（kNoMenu 哨兵）；
          // `menu_bar` 内部关闭路径（Esc/点外/选条目）也经这里传 kNoMenu。
          menu_open_.set(menu_open_.value() == index ? kNoMenu : index);
        },
        {.id = "menubar"});
    // 打开状态 → 下一帧声明面板 overlay（不声明 = 自动消失，框架 sweep）。
    if (menu_open_.value() != kNoMenu && menu_bar_ != nullptr) {
      dsl::menu_panel_overlay(c, *menu_bar_, menu_open_.value());
    }
  }

  /// 文件对话框（打开/导出共用，`file_dialog_mode_` 区分——与 gbcode 的
  /// `build_open_dialog` 同构，含它踩过的两个坑：
  /// ① 标题必须设（无标题顶部空一块，像"内容没加载出来"）；
  /// ② 起始目录只在**一次新的打开动作里**设（每帧无条件 set 会把用户导航
  ///   打回起点——面包屑只有一段、进不去任何目录，实测踩到）。
  void build_file_dialog(dsl::Composer& c) {
    if (!file_dialog_open_.value()) {
      dialog_seen_ = false;   // 不在树上：下次重新打开要重新初始化目录
      return;
    }
    const bool saving = file_dialog_mode_ == 1;
    const bool fresh_open = !dialog_seen_;   // 本帧才被声明 = 一次新的打开动作
    dialog_seen_ = true;
    (void)dsl::overlay(c, "file-dialog", {}, [&] {
      (void)dsl::custom<st::ui::FileDialog>(
          c, [this, saving, fresh_open](st::ui::FileDialog& dialog) {
            dialog.set_id("file-dialog");
            dialog.set_title(saving ? "导出 PNG" : "打开 PNG");
            dialog.set_mode(saving ? st::ui::FileDialog::Mode::Save
                                   : st::ui::FileDialog::Mode::Open);
            if (fresh_open) {
              dialog.set_directory(".");
              if (saving && !document_->file_path().empty()) {
                // 预填当前文件名：改几个字比从零敲快得多（gbcode 同做法）。
                const std::size_t slash = document_->file_path().find_last_of("/\\");
                dialog.set_filename(slash == std::string::npos
                                        ? document_->file_path()
                                        : document_->file_path().substr(slash + 1));
              }
            }
            if (!saving) dialog.set_name_filters({".png"});
            dialog.on_confirm = [this, saving](const std::string& path) {
              file_dialog_open_.set(false);
              if (saving) {
                if (!document_->export_png(path)) {
                  st::log::err("导出失败：{}\n", path);
                }
                return;
              }
              if (!open_file(path)) {
                st::log::err("无法打开：{}\n", path);
              }
            };
            dialog.on_cancel = [this] { file_dialog_open_.set(false); };
          },
          {.id = "file-dialog"});
    });
  }

  void build_toolbar(dsl::Composer& c) {
    dsl::column(c, {.gap = 2.0f, .padding_y = 4.0f, .width = 44.0f,
                    .surface = st::ui::Element::Surface::Alt, .id = "toolbar"}, [&] {
      const Tool tools[] = {Tool::Move,   Tool::Brush, Tool::Eraser, Tool::Bucket,
                            Tool::Eyedropper, Tool::Select, Tool::Crop};
      // AI 接管时工具置灰（「移动」仍可用——那是查看/平移，不是编辑）。
      const bool editable = document_->can_edit();
      for (const Tool tool : tools) {
        const std::string id = std::format("tool-{}", louyue::tool_icon(tool));
        const bool active = canvas_ != nullptr && canvas_->tool() == tool;
        const bool enabled = editable || tool == Tool::Move;
        (void)dsl::custom<Button>(
            c,
            [this, tool, id, active, enabled](Button& b) {
              b.set_id(id);
              b.set_icon(louyue::tool_icon(tool));
              b.set_variant(active ? Button::Variant::Soft : Button::Variant::Ghost);
              b.set_size(Button::Size::Small);
              b.set_enabled(enabled);
              b.on_click = [this, tool] {
                if (canvas_ != nullptr) canvas_->set_tool(tool);
                invalidate_all();
              };
            },
            {.width = 40.0f, .height = 36.0f, .key = id});
      }
    });
  }

  void build_viewport(dsl::Composer& c) {
    // 回流第二跳（拆迁完成）：`CanvasViewport`（内置 Viewport 外壳）包住
    // **文档原生尺寸**的 `CanvasView`。缩放/平移/棋盘格/裁剪全部在外壳；
    // 画布只画位图 + 工具输入 + 文档级动作。
    //
    // 子元素声明用 `custom_container`（ScrollView 内嵌 List 的同款容器形态）：
    // DSL 每帧按游标对齐复用同一个 CanvasView 实例（key="canvas" 命中即复用；
    // 笔刷拖拽进行态/画布指针跨帧保持）。
    //
    // `on_view_changed` 里两件事：① `snap_zoom` 档位吸附（连续值收口——
    // 旧隐疾 #1 根治，协议 set 任意浮点后终值恒在档位表上）；② 触发重组让
    // 状态栏回显跟随（dsl::text 闭包在 build 期求值，不重组就永远旧文案）。
    // 闭包**只捕 this**：shell 是 configure 栈引用（每帧重定义），捕它悬垂。
    CanvasViewport& shell = dsl::custom_container<CanvasViewport>(
        c,
        [&] {
          (void)dsl::custom<CanvasView>(
              c, [this](CanvasView& view) { bind_canvas(view); },
              {.id = "canvas", .key = "canvas"});
        },
        [this](CanvasViewport& outer) {
          outer.set_id("canvas-viewport");
          outer.on_view_changed = [this] {
            if (canvas_ != nullptr) {
              if (auto* live = dynamic_cast<CanvasViewport*>(canvas_->viewport());
                  live != nullptr) {
                live->snap_zoom();   // 吸附后若 zoom 变了会再进本回调一次（收敛于档位值）
              }
            }
            canvas_version_.set(canvas_version_.value() + 1U);   // 重组：状态栏跟显
          };
        },
        {.grow = true, .id = "canvas-viewport", .key = "canvas-viewport"});
    // 画布的坐标换算/缩放真值源指向外壳：在 custom_container **返回之后**接
    //（此时子元素已声明完，父子关系已落）——首帧即生效，不等第二次重组
    //（首帧后若无 State 写入不会再 build，依赖“下一帧补接”会让 e2e 的
    // 首个 zoom_in 落空）。`wants_pan` 同时注入：左键拖拽平移的判定
    //（当前工具 == 移动）是宿主/内容层的知识，外壳只认回调。
    if (canvas_ != nullptr) {
      canvas_->set_viewport(&shell);
      shell.wants_pan = [this] {
        return canvas_ != nullptr && canvas_->tool() == Tool::Move;
      };
    }
    // 视口适配**不在 build 里做**：build 期间视口还没排布（`bounds` 为 0），
    // 在这里算出的适配值无意义——实测它会把 `view_fitted_` 误置成 true，
    // 于是主循环里的适配永远不再发生（海报停在 100%、只看得见一角）。
    // 真正的适配在主循环首帧后（`LouyuePage::fit_view_once`）。
  }

  void bind_canvas(CanvasView& view) {
  view.set_id("canvas");
  view.set_document(document_.get());
  view.on_document_changed = [this] { invalidate_all(); };
  canvas_ = &view;
}

  void build_side_panels(dsl::Composer& c) {
    dsl::column(c, {.gap = 8.0f, .padding = 8.0f, .width = 260.0f,
                    .surface = st::ui::Element::Surface::Alt, .id = "side-panels"}, [&] {
      build_poster_panel(c);
      build_layer_panel(c);
      build_color_panel(c);
    });
  }

  void build_layer_panel(dsl::Composer& c) {
    dsl::column(c, {.gap = 6.0f, .id = "layers-panel"}, [&] {
      dsl::text(c, [] { return std::string("图层"); }, {.id = "layers-title"});
    // 图层列表：**自上而下显示**（面板习惯），而模型是自底向上——此处反转。
    const auto& layers = document_->layers();
    std::vector<dsl::ListItemData> items;
    items.reserve(layers.size());
    for (std::size_t i = layers.size(); i-- > 0;) {
      items.push_back(dsl::ListItemData{
          .key = std::format("layer-{}", i),
          .label = std::format("{} · {}", layers[i].name,
                               st::raster::to_string(layers[i].blend)),
          .subtitle = std::format("{}%", static_cast<int>(layers[i].opacity * 100.0f)),
      });
    }
    (void)dsl::list(
        c, items,
        [this, &layers](std::size_t index) {
          // 显示序是反的：第 `index` 项对应模型里的 `size-1-index`。
          const std::size_t model = layers.size() - 1U - index;
          document_->set_active_layer(model);
          invalidate_all();
        },
        {.id = "layer-list", .key = std::format("layers-{}", layers_version_.value())});

    // 不透明度：调活动图层。
    const float opacity =
        document_->has_layer() ? document_->layers()[document_->active_layer()].opacity : 1.0f;
    (void)dsl::slider(
        c, opacity,
        [this](float v) {
          if (!document_->has_layer()) return;
          document_->layers()[document_->active_layer()].opacity = std::clamp(v, 0.0f, 1.0f);
          canvas_version_.set(canvas_version_.value() + 1U);
          if (canvas_ != nullptr) canvas_->mark_dirty();
        },
        {.id = "opacity-slider", .key = "opacity"})
        .set_enabled(document_->can_edit());

    dsl::row(c, {.gap = 6.0f, .id = "layer-actions"}, [&] {
      (void)dsl::custom<Button>(
          c,
          [this](Button& b) {
            b.set_id("layer-add");
            b.set_icon("plus");
            b.set_variant(Button::Variant::Ghost);
            b.set_size(Button::Size::Small);
            b.on_click = [this] { (void)add_layer(); };
          },
          {.width = 32.0f, .height = 28.0f, .key = "layer-add"});
      (void)dsl::custom<Button>(
          c,
          [this](Button& b) {
            b.set_id("layer-remove");
            b.set_icon("minus");
            b.set_variant(Button::Variant::Ghost);
            b.set_size(Button::Size::Small);
            b.on_click = [this] {
              (void)remove_layer(document_->active_layer());
            };
          },
          {.width = 32.0f, .height = 28.0f, .key = "layer-remove"});
      (void)dsl::custom<Button>(
          c,
          [this](Button& b) {
            b.set_id("layer-merge");
            b.set_icon("layers");
            b.set_variant(Button::Variant::Ghost);
            b.set_size(Button::Size::Small);
            b.on_click = [this] {
              (void)document_->merge_down(document_->active_layer());
              invalidate_all();
            };
          },
          {.width = 32.0f, .height = 28.0f,       .key = "layer-merge"});
      });
    });
  }

  /// **海报面板**（AI 作品面）：人也能自己切控制权、应用/导出，不是只能看。
  ///
  /// 为何需要：AI 接管后人工编辑控件会置灰（画布/图层/颜色），而**这个面板不能跟着灰**
  /// ——它就是人把控制权拿回去的地方。“把手”被锁住等于锁死。
  void build_poster_panel(dsl::Composer& c) {
    dsl::column(c, {.gap = 6.0f, .id = "poster-panel", .key = "poster-panel"}, [&] {
      dsl::text(c, [] { return std::string("海报（AI 作品）"); }, {.id = "poster-title"});
      dsl::row(c, {.gap = 8.0f, .id = "poster-control-row"}, [&] {
        dsl::text(c, [] { return std::string("自动接管"); }, {});
        (void)dsl::switch_(c, document_->ai_controlled,
                           [this](bool on) { set_control(on, "手动切换"); },
                           {.id = "poster-control-switch", .key = "poster-control-switch"});
      });
      dsl::text(
          c,
          [this] {
            return document_->ai_controlled
                       ? std::string("AI 接管中 · 仅可查看")
                       : std::string("人工主导 · 可自由编辑");
          },
          {.size = 12.0f, .id = "poster-control-state", .key = "poster-control-state"});
      dsl::text(
          c,
          [this] {
            return std::format("{} · {}×{} · {} 块", poster_.name, poster_.export_width(),
                               poster_.export_height(), poster_.blocks.size());
          },
          {.size = 12.0f, .id = "poster-meta", .key = "poster-meta"});
      dsl::row(c, {.gap = 6.0f, .id = "poster-actions"}, [&] {
        const bool editable = document_->can_edit();
        (void)dsl::custom<Button>(
            c,
            [this, editable](Button& b) {
              b.set_id("poster-apply");
              b.set_label("示例海报");
              b.set_icon("sparkles");
              b.set_size(Button::Size::Small);
              b.set_variant(Button::Variant::Soft);
              b.set_enabled(editable);
              b.on_click = [this] {
                apply_poster(louyue::builtin_sample_poster());
                poster_script_path_.clear();
              };
            },
            {.key = "poster-apply"});
        (void)dsl::custom<Button>(
            c,
            [this, editable](Button& b) {
              b.set_id("poster-export");
              b.set_label("导出");
              b.set_icon("download");
              b.set_size(Button::Size::Small);
              b.set_variant(Button::Variant::Ghost);
              b.set_enabled(editable);
              b.on_click = [this] {
                const std::string path = "poster.png";
                const std::string error = export_poster(path);
                log(error.empty() ? std::format("已导出：{}", path) : error);
              };
            },
            {.key = "poster-export"});
        (void)dsl::custom<Button>(
            c,
            [this, editable](Button& b) {
              b.set_id("poster-media-refresh");
              b.set_label("刷新素材");
              b.set_icon("refresh");
              b.set_size(Button::Size::Small);
              b.set_variant(Button::Variant::Ghost);
              b.set_enabled(editable);
              b.on_click = [this] {
                const std::size_t count = assets_.refresh();
                log(std::format("素材库：{} 项", count));
                canvas_version_.set(canvas_version_.value() + 1U);
              };
            },
            {.key = "poster-media-refresh"});
      });
      // 素材列表：点一项 → 追加一个 cover 图片块（人也能直接用生成图）。
      if (assets_.size() > 0U) {
        dsl::text(c, [this] { return std::format("素材 {}", assets_.size()); },
                  {.size = 11.0f, .id = "poster-media-title", .key = "media-title"});
        std::vector<dsl::ListItemData> media_items;
        const auto& entries = assets_.entries();
        media_items.reserve(std::min<std::size_t>(entries.size(), 6U));
        for (std::size_t i = 0; i < entries.size() && i < 6U; ++i) {
          media_items.push_back(dsl::ListItemData{
              .key = entries[i].name,
              .label = entries[i].name,
              .subtitle = std::format("{}x{}", entries[i].width, entries[i].height),
          });
        }
        (void)dsl::list(
            c, media_items,
            [this](std::size_t index) {
              if (!document_->can_edit()) return;
              const auto& all = assets_.entries();
              if (index >= all.size()) return;
              louyue::Block block{};
              block.kind = louyue::BlockKind::Image;
              block.name = all[index].name;
              block.image = all[index].name;
              // **插到文字块之前**（图层语义：图片在下、文字在上）。
              //
              // 早期直接 push_back（画在最上层），结果插一张图就把主标题/副标题
              // 盖住了——看着像"渲染坏了"，实际是叠放次序问题。
              // 插在最后一个 Background 之后、第一个文字块之前 = 素材作背景层。
              block.x = 0.5f;
              block.y = static_cast<float>(poster_.height) * 0.30f;
              block.width = static_cast<float>(poster_.width) * 0.40f;
              block.height = static_cast<float>(poster_.height) * 0.26f;
              block.radius = 16.0f;
              block.alpha = 0.55f;   // 作背景层时压淡，文字才读得清
              auto insert_at = poster_.blocks.begin();
              for (auto it = poster_.blocks.begin(); it != poster_.blocks.end(); ++it) {
                const bool is_text = it->kind == louyue::BlockKind::Title ||
                                     it->kind == louyue::BlockKind::Subtitle ||
                                     it->kind == louyue::BlockKind::Body ||
                                     it->kind == louyue::BlockKind::Rule;
                if (is_text) break;
                insert_at = it + 1;
              }
              poster_.blocks.insert(insert_at, block);
              apply_poster(poster_);
            },
            {.id = "poster-media-list",
             .key = std::format("media-{}", canvas_version_.value())});
      }
      // 日志（最近几条；AI 的动作也落在这里，两边看同一份历史）。
      for (const std::string& line : log_lines_) {
        dsl::text(c, [line] { return line; }, {.size = 11.0f});
      }
    });
  }

   void build_color_panel(dsl::Composer& c) {
 dsl::text(c, [] { return std::string("颜色"); }, {.id = "color-title"});
 const Color color = canvas_ != nullptr ? canvas_->foreground() : Color{0, 0, 0, 255};
 dsl::row(c, {.gap = 8.0f, .id = "swatch-row"}, [&] {
 // 色块：**自绘元素**（`Swatch`）——不用 `Panel` + `style().background`，
 // 理由见 `Swatch` 定义处（`apply_theme` 每帧覆盖调用方写的背景色）。
 (void)dsl::custom<Swatch>(c, [color](Swatch& swatch) { swatch.set_color(color); },
 {.id = "swatch"});
 dsl::text(c,
 [color] { return std::format("#{:02X}{:02X}{:02X}", color.r, color.g, color.b); },
 {.id = "hex-label", .key = "hex"});
 });
 // 取色：内置 `ColorPicker`（36 轮回流决策）——手写 HSV 三滑杆曾是它的
 // 源头（见 color_picker.hpp 文件头），现在回到示例里成为它的**应用示范位**。
 // 组件自带色相条/SV 块/十六进制输入，不接吸管源（`set_eyedropper_source`）——
 // 无头下没有屏幕可截，让它如实禁用。
 // 颜色链路不变：`on_change` → `canvas_->set_foreground`（画笔真值源）。
 // **每帧把画布前景色同步进控件**（`notify=false`：同步不触发回调，
 // 否则与用户操作混淆）；画布前景色只有本面板能改，两者实际上同源，
 // 这一句主要是给初始色对齐（控件默认黑，画布初始 #20222C）。
 (void)dsl::custom<st::ui::ColorPicker>(
 c, [this, color](st::ui::ColorPicker& picker) {
 picker.set_id("color-picker");
 if (picker.color() != color) picker.set_color(color, false);
 picker.on_change = [this](st::math::Color picked) {
 if (canvas_ != nullptr) canvas_->set_foreground(picked);
 // 触发重组让 swatch/hex-label 跟色（闭包按值捕获前景色，不标脏不会重捕；
 // 与 opacity 滑块的 on_change 同款）。
 canvas_version_.set(canvas_version_.value() + 1U);
 if (canvas_ != nullptr) canvas_->mark_dirty();
 };
 },
 {.id = "color-picker", .key = "picker"});
 }

  void build_status_bar(dsl::Composer& c) {
    dsl::row(c, {.gap = 16.0f, .padding_x = 10.0f, .height = 26.0f,
                 .surface = st::ui::Element::Surface::Alt, .id = "status"}, [&] {
      dsl::text(
          c,
          [this] {
            if (canvas_ == nullptr) return std::string("—");
            const auto pos = canvas_->cursor_image_pos();
            return std::format("{},{}", static_cast<int>(pos.x), static_cast<int>(pos.y));
          },
          {.id = "status-coords", .key = "coords"});
      dsl::text(
          c, [this] { return std::format("{}×{}", document_->width(), document_->height()); },
          {.id = "status-size", .key = "size"});
      dsl::text(
          c,
          [this] {
            // 回流第二跳：缩放真值在外层视口（档位吸附后的终值）。经 canvas_
            // 间接取（闭包不捕栈引用 shell；canvas_ 由 bind_canvas 每帧刷新）。
            const float zoom = canvas_ != nullptr ? canvas_->zoom() : 1.0f;
            return std::format("{:.0f}%", static_cast<double>(zoom) * 100.0);
          },
          {.id = "status-zoom", .key = "zoom"});
      dsl::text(
          c,
          [this] {
            return std::string(canvas_ != nullptr ? louyue::tool_name(canvas_->tool()) : "-");
          },
          {.id = "status-tool", .key = "tool"});
      dsl::text(
          c,
          [this] {
            return std::format("{} 步历史", document_->history_size());
          },
          {.id = "status-history", .key = "history"});
    });
  }

   std::unique_ptr<Document> document_{};
 CanvasView* canvas_{nullptr};

 // —— 海报（AI 作品面；见 poster.hpp / poster_bridge.hpp）——
 louyue::PosterScript poster_{louyue::builtin_sample_poster()};
 /// 素材库（海报的 image 块靠它解析与解码）。
 louyue::AssetLibrary assets_{};
 std::string asset_basis_{};
 std::string poster_script_path_{};
 std::string last_export_{};
 const st::ui::TextPort* text_port_{nullptr};
 /// 等画布排布完成后适配视口（见 `apply_poster` 的注释）。
 bool fit_pending_{false};
  /// 是否已按视口尺寸同步过一次（只做一次，避免冲掉人工改动）。
  bool view_fitted_{false};
  /// 这次 `apply_poster` 来自自动同步（不重置 `view_fitted_`，否则会死循环重渲染）。
  bool data_override_{false};

 public:
  /// 是否已完成首次视口适配（主循环判据）。
  [[nodiscard]] auto is_view_fitted() const noexcept -> bool { return view_fitted_; }

  /// 把海报**按视口可用尺寸**渲染一次（首帧之后）。
  ///
  /// ⚠ 为何不用 `Viewport::fit_to_view`：它读 `content_size()`，而子元素
  /// （`CanvasView`）的尺寸要等它自己 `measure` 过才有值；换文档后即便先
  /// `mark_layout_dirty()`，`content_size()` 仍是旧值——实测 view=876x662 /
  /// doc=1920x1080，调用后 `zoom` 纹丝不动（仍 1.00）。
  ///
  /// 改成**换渲染尺寸**而不是换缩放：视口多大就把海报渲染多大（保持宽高比，
  /// 留 8px 边），于是 100% 缩放天然装得下，用户还能照常用滚轮放大看细节。
  ///
  /// 只做一次（`view_fitted_`）：之后用户改窗口/缩放都不再重渲染——重渲染会
  /// 丢掉人工在画布上做的任何像素修改。
  bool sync_poster_to_viewport() {
    if (view_fitted_ || canvas_ == nullptr || data_override_) return false;
    auto* shell = dynamic_cast<CanvasViewport*>(canvas_->viewport());
    if (shell == nullptr) return false;
    const float view_w = shell->bounds().width - 16.0f;
    const float view_h = shell->bounds().height - 16.0f;
    if (view_w < 64.0f || view_h < 64.0f) return false;   // 还没排布
    // 保持海报宽高比，缩到视口内。
    const float ratio = static_cast<float>(poster_.width) / static_cast<float>(poster_.height);
    int target_w = static_cast<int>(view_w);
    int target_h = static_cast<int>(static_cast<float>(target_w) / ratio);
    if (static_cast<float>(target_h) > view_h) {
      target_h = static_cast<int>(view_h);
      target_w = static_cast<int>(static_cast<float>(target_h) * ratio);
    }
    target_w = std::clamp(target_w, 160, 3840);
    target_h = std::clamp(target_h, 160, 3840);
    if (target_w == poster_.export_width() && target_h == poster_.export_height()) {
      // 尺寸没变也要把内容居中——启动时 pan 停在旧偏移上，画布会偏出半屏
      // （实测：海报完整、但视口里只看到下半张）。
      shell->set_zoom(1.0f);
      shell->set_pan(st::math::Point{0.0f, 0.0f});
      view_fitted_ = true;
      canvas_version_.set(canvas_version_.value() + 1U);
      return false;
    }
    poster_.out_width = target_w;
    poster_.out_height = target_h;
    view_fitted_ = true;
    data_override_ = true;   // 这是一次自动同步，不是用户改脚本
    apply_poster(poster_);
    data_override_ = false;
    // 画布尺寸正好等于视口可用区 → zoom 1.0 + pan 0 就是居中。
    if (auto* fresh = dynamic_cast<CanvasViewport*>(canvas_->viewport()); fresh != nullptr) {
      fresh->set_zoom(1.0f);
      fresh->set_pan(st::math::Point{0.0f, 0.0f});
    }
    canvas_version_.set(canvas_version_.value() + 1U);
    return true;
  }


 /// 海报渲染成图层 → 换掉文档（详见 `apply_poster_text` 的注释）。
 void apply_poster(const louyue::PosterScript& poster) {
   poster_ = poster;
   const int width = poster_.export_width();
   const int height = poster_.export_height();
   document_ = std::make_unique<Document>(width, height);
   st::raster::Canvas canvas{width, height, 1.0f};
   louyue::render_poster(canvas, poster_, text_port_, &assets_, asset_basis_);
   st::codec::PngImage image;
   image.width = static_cast<std::uint32_t>(width);
   image.height = static_cast<std::uint32_t>(height);
   image.rgba = canvas.to_rgba8();
   (void)document_->load_png(image);
   document_->set_dirty(false);
   canvas_version_.set(canvas_version_.value() + 1U);
   layers_version_.set(layers_version_.value() + 1U);
   if (canvas_ != nullptr) {
     canvas_->set_document(document_.get());
     canvas_->mark_dirty();
     fit_pending_ = true;
     // 自动同步（`data_override_`）不算"用户换了作品"，不要重置已同步标记，
     // 否则 `sync_poster_to_viewport` 会再进来一次、无限重渲染。
     if (!data_override_) view_fitted_ = false;
   }
   // 日志只报**作品设计尺寸**：画布尺寸是"看"的尺寸（随视口变），报它没意义。
   log(std::format("海报：{} · {} 块 · {}×{}", poster_.name, poster_.blocks.size(),
                   poster_.width, poster_.height));
 }

 /// 海报脚本里的相对路径按脚本所在目录重基（现在没有外部素材，留位子以防日后加图片块）。
 void rebase_poster_paths(louyue::PosterScript& poster) const { (void)poster; }

 void log(std::string line) {
   log_lines_.push_back(std::move(line));
   if (log_lines_.size() > 40) log_lines_.erase(log_lines_.begin());
   canvas_version_.set(canvas_version_.value() + 1U);
 }

  // —— 菜单 / 文件对话框（打开·导出入口，见 build_menu_bar/build_file_dialog）——
  static constexpr std::size_t kNoMenu{static_cast<std::size_t>(-1)};
  /// 当前打开的下拉菜单（`kNoMenu` = 无）。**必须是 State**：点击标题只发生在
  /// 事件分发里，不标脏就不会触发下一帧重组——面板 overlay（在 build 里声明）
  /// 永远不会出现（gbcode 的 MenuHeader 同款坑）。
  dsl::State<std::size_t> menu_open_{kNoMenu};
  st::ui::MenuBar* menu_bar_{nullptr};      ///< 每帧 build 重新取得（非拥有）
  dsl::State<bool> file_dialog_open_{false};
  int file_dialog_mode_{0};                 ///< 0 = 打开 PNG；1 = 导出 PNG
  bool dialog_seen_{false};                 ///< 对话框是否在树上（新开动作判据）
  bool quit_requested_{false};              ///< 菜单「退出」（主循环退出位）
    dsl::State<std::size_t> canvas_version_{0};
 dsl::State<std::size_t> layers_version_{0};
 std::vector<std::string> log_lines_{};
};

}  // namespace

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);

  st::app::AppOptions app_options;
  app_options.width = 1180;
  app_options.height = 760;
  app_options.title = "镂月 Lunaris · 霜天";
  app_options.headless = options.headless;
  app_options.backend = !options.backend.empty() ? options.backend
                        : (options.headless ? std::string("headless") : std::string{});
  app_options.theme = options.theme == "dark" ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light;
  app_options.control_port = options.control_port;
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;

  st::app::Application app("louyue", "0.1.0", app_options);
  // **自绘组件必须先注册**（在 `custom<T>` 用它之前，即建界面之前）。
  // `CanvasView` 是示例级类型，不进框架组件表（那会让框架的公开组件面
  // 被具体示例的需求撑大）；`register_element_type` 就是为此留的口子。
  if (!dsl::register_element_type<CanvasView>("CanvasView")) {
    std::fprintf(stderr, "CanvasView 注册失败（与内置组件重名？）\n");
    return 1;
  }
  // 回流第一跳的外壳（内置 Viewport 子类）也要登记——`custom<T>` 走注册表
  // 创建（dsl.hpp：未注册直接 abort）。
  if (!dsl::register_element_type<CanvasViewport>("CanvasViewport")) {
    std::fprintf(stderr, "CanvasViewport 注册失败（与内置组件重名？）\n");
    return 1;
  }
  if (!dsl::register_element_type<Swatch>("Swatch")) {
  std::fprintf(stderr, "Swatch 注册失败（与内置组件重名？）\n");
  return 1;
}
// 海报控制面（AI 作品级入口）：与 CanvasView 同理，是示例级自绘元素。
if (!dsl::register_element_type<louyue::PosterBridge>("LouyuePoster")) {
  std::fprintf(stderr, "PosterBridge 注册失败（与内置组件重名？）\n");
  return 1;
}
  auto page = std::make_shared<LouyuePage>();
  if (!options.open.empty()) {
    if (!page->open_file(options.open)) {
      std::fprintf(stderr, "无法打开 %s\n", options.open.c_str());
    }
  }

  // 外壳：`WindowFrame`（标题栏 + 内容槽 + 八向缩放边缘），与其他示例同形态。
  auto frame = std::make_unique<st::ui::WindowFrame>("镂月 Lunaris · 霜天");
  frame->set_id("window-frame");
  frame->set_window_control(&app);
  if (frame->title_bar() != nullptr) {
    frame->title_bar()->set_id("titlebar");
    frame->title_bar()->set_icon("brush");
  }
  st::ui::Element* slot = frame->content();
  app.set_content(std::move(frame));
  auto host = dsl::mount_into(app.root(), *slot, page);
  if (host == nullptr) {
    std::fprintf(stderr, "声明式挂载失败\n");
    return 1;
  }
  // **重组错误必须可见**（承 gbcode 的做法）：单根契约被破坏、护栏报表或
  // 预算耗尽时界面会静默地少一块——那时看截图只能看到“一片空白”。
  {
    const dsl::ReconcileStats& stats = host->stats();
    if (!stats.error.empty()) {
      std::fprintf(stderr, "[louyue] 声明式重组错误：%s\n", stats.error.c_str());
    }
    for (const auto& collision : stats.key_collisions) {
      std::fprintf(stderr, "[louyue] 重名 key：%s\n", collision.c_str());
    }
  }

  // 首帧重组**必须在 `start()` 之前**：`start()` 内会开控制通道，
  // 客户端一看到控制文件就连——那时界面还没建好就会读到半成品状态。
  (void)host->tick();

  // ⚠ 控制通道是在 `Application::start()` 里起的（不在 `tick()` 里）。
  // 自定义主循环（下面是 gallery 同款）必须**显式调 `start()`**——
  // 漏了它的表现是“进程在跑、界面也在画，但控制文件永远不会出现”，
  // 从外部看就是“启动超时”（实测踩到）。
  if (auto status = app.start(); !status) {
  std::fprintf(stderr, "启动失败：%s\n", status.error().message.c_str());
  return 1;
}
// **必须在 start() 之后**：`root().text_port()` 要等应用初始化完成才可用。
// 没有这一步的后果不是报错，而是**海报里一个字都没有**——文字层静默跳过（实测）。
// 素材根用**绝对路径**：相对路径按 cwd 解析，而无头/双击/CI 的 cwd 各不相同
// （实测：相对路径导致素材列表为空，看着像"素材面板没做"）。
// 从 argv[0] 推导：可执行文件在 <repo>/shuangtian/build/<档>/bin/，素材在 <repo>/shuangtian/examples/。
if (argv[0] != nullptr) {
  std::string asset_dir = st::fs::parent(std::string(argv[0]));
  for (int i = 0; i < 3 && !asset_dir.empty(); ++i) asset_dir = st::fs::parent(asset_dir);
  if (!asset_dir.empty()) {
    page->set_asset_root(st::fs::join(asset_dir, "examples/caiyun/assets/media"));
  }
}
page->set_text_port(app.root().text_port());

  // 主循环形态**承 gallery**：需要每帧显式推进声明式宿主
  // （`tick_declarative_hosts`）——`DeclarativeHost` 登记的帧回调里已经内建
  // “先 pump_async 再判 dirty”的顺序，调用方不再重写（见 gallery 同段注释）。
  std::uint64_t frames = 0;
while (!app.quit_requested() && !page->quit_requested()) {
  const std::int64_t frame_start_ms = st::time::now_ms();
  if (app.root().tick_declarative_hosts() > 0U) app.request_repaint();
  // **海报按视口尺寸渲染一次**：视口几何只有在首帧排布之后才有效（`build()` 期间
  // `bounds` 是 0），所以这件事必须在主循环里做。做完就置位、不再重做
  //（否则用户自己缩放/改窗口都会被强制重渲染，白丢编辑）。
  if (!page->is_view_fitted() && page->sync_poster_to_viewport()) app.request_repaint();
  app.tick();
  ++frames;
  app.pace_loop(frame_start_ms);
}
  st::log::out("镂月退出：{} 帧，DPI {:.1f}，后端 {}\n", frames,
            static_cast<double>(app.device_scale()), std::string(app.backend_name()));
  return 0;
}

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页。
ST_MAIN(run_app)
