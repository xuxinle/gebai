# 霜天 Backlog（2026-09-30 全面审视后的待办项）

> 来自《霜天全面审视报告》第三梯队与长期项。本文件是唯一权威清单，完成后移除条目并在 DESIGN.md 更新里程碑。

## P1（下轮会话优先）

- [ ] **x11 窗口后端**：backend.cpp:309-323 现仅探测+Unsupported，Linux 真桌面硬阻塞。Backend 15 虚函数接口已稳定，纯窗口层工作量（文档自评）。
- [ ] **IME 输入法**：win32 无 WM_IME_* 处理，中文输入在窗口模式不可用（无头 AI 注入不受影响）。EventKind 需加合成事件。
- [ ] **事件流 TS 端消费**：A1 已做常驻连接与事件队列；待补 `shuangtian_wait_event` 工具暴露给子代理 + 服务端 `ui.changed` 携带变更元素 id 清单（§6.1 示例语义）。
- [ ] **视觉断言原语**：`visual.diff` / `capture.hash`（区域像素哈希/模板相似度）进协议——「改代码→看图→断言」闭环最后一公里；顺带解决截图像素不确定导致的回归难。
- [ ] **停摆竞态根因定位**：A1 已修头号嫌疑（errno 误分类）并加固（失败不静默/心跳可探）；若探针压力下仍复现，用 st_decisive_probe.py 取证路径继续查（复现表征见审视报告 §4.1）。

## P2

- [ ] 多窗口抽象（Window/WindowManager 进 shell，control::Host 带窗口维度）——越晚做上层耦合越多。
- [ ] 统一动画系统（现每组件手工状态机：advance_hover/last_hover_time_）。
- [ ] DisplayList（立即模式→保留模式）：解锁多线程分帧、录制回放测试、GPU 图层缓存（v0.3 前置）。
- [ ] invoke 动作面文档同步（A1 已白名单化，DESIGN §6.2 需列全）。
- [ ] 协议命令注册表机制（server.cpp 线性 if-else 966 行，新命令三处同步）。
- [ ] `st add/fetch/vendor/audit/outdated` CLI 入口（内核已就绪：semver/求解器/校验/固化）+ TLS（schannel/openssl dlopen）+ 归档解包接 codec——或文档持续标注「规划中」。
- [ ] 无障碍桥（UIA/AT-SPI/AXUIElement）：SemanticsNode 数据已在，建议从「永久非目标」改为规划项。
- [ ] GPOS/RTL/bidi/HarfBuzz 类 shaping 层（当前仅 kern format0，多语言排版不可达）。
- [ ] Scene3D GPU 腿（软件腿已可用；触发条件见 DESIGN §8.4.2）+ 多线程光栅化。
- [ ] 文件对话框/全局快捷键/系统托盘等桌面系统能力。
- [ ] 空闲 CPU 回归护栏（60s 窗口量进程 CPU；§8.4.4 忙循环教训的机制化）。
- [ ] check_docs.py 强化为数字/组件清单/目录引用自动校验（本次已修全部已知漂移；机制防复发待做）。

## 已完成（本轮「全部优化」落地，备查）

- [x] 网络层错误码平台化 + 发送失败不静默 + 帧合并单发 + Nagle + 接收缓冲游标（A1）
- [x] 鉴权最小集（token 写控制文件 + hello 校验 + capture 路径白名单）（A1）
- [x] 协议补全：drag / wait frames / invoke 白名单 / 崩溃自愈 crash handler + 控制文件清理（A1）
- [x] 协议一致性测试 + TS 端 ping 判活/孤儿回收/常驻事件流（A1）
- [x] 增量重绘（element 级 damage）+ 整形缓存 + 字形 LRU + 渐变 LUT + AVX2 + vsync/空闲阻塞（A2）
- [x] 链接指纹缓存 + MSVC PCH + Windows 内存探测 + st clean + per-case 超时/junit + lint regex + bootstrap 去双写（A3）
- [x] CI 三平台矩阵 + mingw 交叉（.github/workflows/shuangtian-ci.yml）
- [x] 文档漂移全修（DisplayList/vendor/组件清单/OpenGL/§8.4.2/数字/错误码）（阶段 C）
