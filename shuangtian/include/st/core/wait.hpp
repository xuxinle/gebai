#pragma once

#include <cstdint>
#include <span>

/// 平台级高精度等待原语。
namespace st::platform {

/// 睡眠（毫秒，可为小数；<=0 立即返回）。
///
/// 为什么不用 `std::this_thread::sleep_for`：**Windows 默认定时器粒度 15.6ms**——
/// `sleep_for(4ms)` 实际睡 ~15.6ms。应用主循环用它做空闲轮询（4ms）与帧预算节流时，
/// 控制通道命令的响应节拍被拉到 ~31ms（实测 ping p50=31.2ms，而服务端处理只有
/// 几百微秒）——「AI 驱动」的每一次调用都在付这笔看不见的税。
///
/// 实现：Windows 优先 `CreateWaitableTimerEx(HIGH_RESOLUTION)`（Win10 1803+，
/// **进程级**精度提升，不像 `timeBeginPeriod` 那样拉高全系统功耗；每线程一个定时器）；
/// 老系统/失败回退 `Sleep`。POSIX 用 `nanosleep`（天然亚毫秒粒度）。
void sleep_ms(double milliseconds);

/// 等待一个平台句柄（POSIX fd / Windows SOCKET）可读（或可写）。
///
/// 与 `sleep_ms` 的分工：这是**可被打断的等待**——主循环把控制通道的句柄
/// 交给内核，新请求一到就立即返回，而不是盲睡固定拍再去看（后者会把命令落地时刻
/// 量化到节拍边界，实测每次白付 ~12ms）。`timeout_ms < 0` = 无限等待。
///
/// `handle` 原样传平台 API（POSIX 是 fd、Windows 是 SOCKET），调用方负责保证其有效。
[[nodiscard]] auto wait_handle(std::intptr_t handle, bool readable, int timeout_ms) -> bool;

/// 等待**任意一个**句柄可读（一次 `poll`/`WSAPoll` 覆盖全部句柄）。
///
/// 为什么需要多句柄版：控制通道有**两类**可能会先到的数据源——监听套接字
/// （新连接）与**每个已连接的客户端套接字**（请求）。只等监听套接字时，
/// “客户端发来请求”这类事件根本不会唤醒等待（实测踩到：改完等待机制延迟纹丝不动，
/// 因为数据到达的是已接受的那个连接）。
///
/// 返回是否有句柄就绪；`handles` 为空时直接返回 false（调用方自行退化成睡眠）。
[[nodiscard]] auto wait_any_readable(std::span<const std::intptr_t> handles, int timeout_ms)
    -> bool;

}  // namespace st::platform
