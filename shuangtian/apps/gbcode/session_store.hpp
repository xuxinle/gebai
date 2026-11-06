#pragma once

/// gbcode 的会话持久化（最近打开 + 窗口关闭时的会话恢复）。
///
/// **纯数据层**：不依赖 UI（可单测），路径与行号都是普通值。UI 侧（page_actions /
/// main）在动作发生时记录、在退出时保存、在启动时恢复。
///
/// 落盘位置：`{ST_HOME 或 ~/.shuangtian}/gbcode/session.json`——与框架的
/// `~/.shuangtian/cache` 同根（用户数据都归一个家，不散落）。
///
/// 失败安全姿态：文件缺失/损坏/字段类型不符 → 返回空态（`load` 不抛、不喊），
/// "上次会话丢了"不该让本次启动失败；单测覆盖这三条路径。

#include <cstddef>
#include <string>
#include <vector>

namespace gbcode {

/// 会话里一个打开过的文件（路径 + 关窗时的光标位置，1 起；恢复时跳过去）。
struct SessionFile {
  std::string path{};
  std::size_t line{1};
  std::size_t column{1};
};

/// 完整会话（一次窗口生命周期的最后快照 + 跨会话的最近清单）。
struct SessionState {
  /// 关窗时的工作区（空 = 内置样例模式，恢复时跳过）。
  std::string workspace{};
  /// 关窗时打开的文件标签（按标签顺序）。
  std::vector<SessionFile> files{};
  /// 关窗时的活动标签下标（越界时恢复侧夹取）。
  std::size_t active{0};
  /// 最近打开的文件（MRU：队首最新；去重、上限 10）。
  std::vector<std::string> recent_files{};
  /// 最近打开的文件夹（换工作区记录；MRU 同上）。
  std::vector<std::string> recent_folders{};
};

/// 会话文件的默认位置（`{ST_HOME:-~/.shuangtian}/gbcode/session.json`）。
[[nodiscard]] auto default_session_path() -> std::string;

/// 读取会话。文件不存在/损坏/字段类型不符 → 空态（失败安全，见文件头）。
/// 恢复侧对拿到的路径还有一道 `fs::exists` 校验——这里不做（数据层不碰磁盘真相，
/// "上次打开的文件还在不在"是恢复时的事）。
[[nodiscard]] auto load_session(const std::string& path) -> SessionState;

/// 写出会话（先确保父目录存在）。失败返回 false（调用方写状态栏，不弹窗）。
[[nodiscard]] auto save_session(const std::string& path, const SessionState& state) -> bool;

/// MRU 头插：已在清单里则提到队首，超出上限截断（返回后清单长度 ≤ limit）。
/// 空路径直接忽略（内置样例没有磁盘身份，记了也没法恢复）。
auto push_recent(std::vector<std::string>& recents, std::string path,
                 std::size_t limit = 10) -> void;

}  // namespace gbcode
