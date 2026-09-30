# 霜天 Backlog（2026-09-30 全面审视与「全部优化」会话后的台账）

> 本文件是唯一权威清单，完成后移入「已完成」并在 DESIGN.md 更新里程碑。
> **诚实原则**：写着「待做」却已完成的条目会误导读者；写着「已完成」却没落地的条目更糟。

## P0（性能地基，最高优先——本轮渲染侧四大项未落地）

- [ ] **增量重绘（脏区重绘）**：`dirty_rect_` 目前只有整视口口径，每帧全树重绘。
  实测软件腿 2×DPI 全量帧 ≈39 ms（超 60fps 预算 2.4×）——「高性能」最大单项。
  修法：element 级 damage 累积（mark_dirty 时并入 bounds 到 dirty rect 并），
  软件路径先受益（无需 DisplayList）；验收：2×DPI 小脏区帧 < 8 ms + 像素不变式测试。
- [ ] **文本整形结果缓存**：`TextRenderer::draw` 每次调用 `shape()`（每帧全量重整形）；
  按（文本+字号+role）键控缓存，失效点只有字体栈变更（§4.3 声称「缓存：整形结果」尚未实现）。
- [ ] **字形缓存 LRU**：`trim_cache` 超限时**全清**（非 LRU）——CJK 大文档滚动周期性帧尖峰。
- [ ] **渐变 LUT 快路径**：渐变逐像素采样（bench 实测 162× 清屏单位，最贵原语）；
  参照 GPU 侧 ramp texture 做法预计算 ramp。
- [ ] **帧调度工程化**：win32 窗口模式 Present(1)/DwmFlush vsync；空闲态
  MsgWaitForMultipleObjectsEx/WaitMessage 事件阻塞（现为 4ms 轮询常驻唤醒）；
  Windows timeBeginPeriod(1) 配对。附：空闲 CPU 回归护栏（60s 窗口量进程 CPU）。

## P1

- [ ] **x11 窗口后端**：backend.cpp 现仅探测+Unsupported，Linux 真桌面硬阻塞。
- [ ] **IME 输入法**：win32 无 WM_IME_* 处理，中文输入在窗口模式不可用
  （无头 AI 注入不受影响）。EventKind 需加合成事件。
- [ ] **停摆竞态根因定位**：2026-09-30 实测两次「单连接永久停摆」（服务端健康、
  仅旧连接饿死、>60s 不自愈；两次命中后 ~4300 次调用未再复现）。已加固：
  错误码平台化（头号嫌疑：winsock 不设 errno）、发送失败不静默、心跳可探。
  若再复现：用 tools/ 下探针的取证路径继续查（表征见审视报告 §4.1）。
- [ ] **视觉断言原语**：`visual.diff` / `capture.hash`（区域像素哈希/模板相似度）进协议——
  「改代码→看图→断言」闭环最后一公里。
- [ ] **事件流 TS 端完整消费**：客户端常驻连接与事件队列已做；
  待补：`shuangtian_wait_event` 工具暴露 + 服务端 `ui.changed` 携带变更元素 id 清单。

## P2

- [ ] 多窗口抽象（Window/WindowManager 进 shell，control::Host 带窗口维度）。
- [ ] 统一动画系统（现每组件手工状态机：advance_hover/last_hover_time_）。
- [ ] DisplayList（立即模式→保留模式）：解锁多线程分帧、录制回放测试、GPU 图层缓存。
- [ ] 协议命令注册表机制（server.cpp 分发链较长，新命令多处同步）。
- [ ] `st add/fetch/vendor/audit/outdated` CLI 入口（内核已就绪）+ TLS + 归档解包。
- [ ] 无障碍桥（UIA/AT-SPI/AXUIElement）：SemanticsNode 数据已在，建议列入规划。
- [ ] GPOS/RTL/bidi/HarfBuzz 类 shaping 层（当前仅 kern format0）。
- [ ] Scene3D GPU 腿（软件腿已可用；触发条件见 DESIGN §8.4.2）+ 多线程光栅化。
- [ ] 文件对话框/全局快捷键/系统托盘等桌面系统能力。
- [ ] check_docs.py 纳入 CI 常跑（本轮已升级为事实核对；防复发机制已建）。

## 已完成（本轮「全部优化」落地，备查）

- [x] **网络层错误码平台化**（WSAGetLastError/errno 分流）+ **发送失败不静默** + 帧合并单发 +
  accept 关 Nagle + 接收缓冲偏移游标（A1）
- [x] **鉴权 token 最小集**（自动生成→控制文件→hello 校验→未握手只允许 hello/ping）+
  **capture 落盘白名单**（A1）
- [x] **协议补全**：input.mouse drag / wait for=frames / invoke 动作白名单 /
  key press 补 KeyUp / wait elapsed_ms 修正（A1）
- [x] **崩溃自愈**：platform_crash.cpp（SEH/信号 → stderr 现场）+ ST_MAIN 自动安装 +
  退出清理控制文件（A1）
- [x] **协议一致性测试**（control_protocol_test.cpp，11 用例）+ TS 客户端**自动握手**
  （同批写出不增往返）+ ping 判活/孤儿回收/PID 复用防护 + TS 测试平台化修复（38/38 绿）（A1）
- [x] **AVX2 运行时分派**（simd.cpp 16 通道 + __cpuid 门控 + 单文件 /arch:AVX2）（A2）；
  附修两个预存缺陷：MSVC 因宏缺失**静默退化标量**、SIMD 除 255 近似 **off-by-one**
  （改精确公式 `(x*0x8081)>>23`）
- [x] **链接指纹缓存**（无改动 dev 构建 3.3s→1.6s，链接直接跳过）；**MSVC 预编译头**；
  **Windows 内存探测**（GlobalMemoryStatusEx）；**st clean**；**测试框架** per-case 超时
  + `--list` + `--format junit`（并完成 CLI 接线）；lint 正则一次性编译；
  bootstrap.ps1 读 st.pkg 去双写（A3）
- [x] **CI 三平台矩阵**（.github/workflows/shuangtian-ci.yml：linux/windows/macos
  test+lint，Linux 附 mingw 交叉编译）
- [x] **文档漂移全修** + check_docs.py v2（§ 引用/路径实存/组件清单/用例数四类自动核对）

> 审视报告「第一梯队」全部与「第二梯队」的构建侧已在上述条目落地；
> **第二梯队的渲染侧（增量重绘/整形缓存/LRU/渐变 LUT/帧调度）本轮未落地**，
> 列为本文件 P0——是下一会话的第一优先项。
