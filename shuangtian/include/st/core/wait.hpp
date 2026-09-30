#pragma once

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

}  // namespace st::platform
