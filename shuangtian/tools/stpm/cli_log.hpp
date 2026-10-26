/// `st` 的运行日志：每次 build / test / lint 把**完整输出**落盘，并允许事后按需读取。
///
/// ## 为什么在 `st` 里做，而不是在包装脚本里
///
/// 包装脚本只能覆盖「愿意用脚本的人」；而构建/测试是**每个**开发路径都要走的，
/// 包括直接敲 `st build`、通过 IDE、通过无头智能体驱动。放进 `st` 之后：
/// 无论谁、怎么调用，日志都已存在——事后要查时不用重现。
///
/// 实测代价（本仓库）：`st build` 因 `duplicate symbol` 失败，而日志文件里只有一行
/// “日志落盘：…”；真正的 `ld.lld: error: …` 一行没有 → 必须重跑一遍才能看见。
///
/// ## 路径规则（刻意可预测）
///
/// `<工程根>/.st/logs/<action>-<target>-<profile>-<YYYYmmdd-HHMMSS>-<pid>.log`
///
/// - 前缀是 action（build/test/lint/rebuild），所以 `ls .st/logs/build-*` 就是全部构建日志；
/// - 时间是**文件名里**的，不靠 mtime 排序（拷来拷去之后 mtime 会变，名字不会）；
/// - 带 pid：同秒并发两次也不会互相覆盖（历史消息里引用过的产物不能变内容）。
///
/// ## 与 `st::log` 落盘的分工
///
/// `st::log` 写结构化日志事件（`build/bin/logs/st.log`，供崩溃回溯）；
/// 这里写的是**控制台原样输出**（`st::print`/`st::eprint` 的完整副本），
/// 供人/智能体事后 grep。两者都保留，各司其职。
#pragma once

#include <string>
#include <string_view>

namespace st::cli_log {

/// 目录名（相对工程根）：`.st/logs`。单独导出是为了让 `st log` 与写入方口径一致。
inline constexpr std::string_view kDirectory{".st/logs"};

/// 是否关闭落盘（`--no-log` / 环境变量 `ST_NO_CONSOLE_LOG=1`）。
///
/// 存在的理由：极少数场景（只读文件系统、日志目录不可写、基准测量要排除 IO 影响）
/// 需要关掉它。默认**开**——"事后查得到"比"省几毫秒"重要。
[[nodiscard]] auto suppressed() -> bool;

/// 开始一次运行：算路径、打开镜像、打印一行"日志：<path>"（这一行本身也在日志里）。
///
/// @param root    工程根（日志落在 `root/.st/logs/`）
/// @param action  命令名（build / test / lint / rebuild / check）
/// @param target  目标名（空 → `lib`）
/// @param profile 档位（空 → `-`）
/// @return 日志文件路径；未开启/打开失败时返回空串
[[nodiscard]] auto begin(std::string_view root, std::string_view action,
                         std::string_view target, std::string_view profile) -> std::string;

/// 结束一次运行：写一行收尾（耗时/退出码）并关闭镜像。
void end(int exit_code);

/// 当前日志路径（未开启时为空）。
[[nodiscard]] auto current_path() -> std::string;

}  // namespace st::cli_log
