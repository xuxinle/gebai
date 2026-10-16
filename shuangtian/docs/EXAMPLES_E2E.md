# 示例 e2e 入口表

> `examples/` 五个示例是框架的对外脸面，长期缺巡检必腐化。本表收「直接驱动示例二进制、
> 走控制通道、带断言」的端到端脚本——**新增示例或新 e2e 脚本时同步更新本表**，
> 别让后来者再靠 grep 摸清单。

## 前置

- 所有脚本默认驱动 `build/dev/bin/<例>.exe`（例外在表中注明），先 `st build <例>`；
- `py -3` 为 Windows 写法，Linux/macOS 等价 `python3`（下表统一写 `py -3`）；
- 产物落点：各脚本在仓库 `tmp/<例>-*-shots/` 落控制文件（ctl.json）/日志/截图，
  互不污染（gbcode 系已把 `ST_HOME` 重定向到当次产物目录，避免会话恢复互相踩）。

## 入口表

| 脚本（tools/ 下） | 示例 | 覆盖面 | 跑法 |
|---|---|---|---|
| `st_visual_check.py` | gallery + gbcode | dev + san 双档完整交互序列：查询/操作/输入/DPI 切换/主题切换，截图落盘供人眼核验 | `py -3 tools/st_visual_check.py` |
| `st_editor_smoke.py` | gbcode / gallery | 编辑器形态三断言：点击即聚焦、文本送达焦点元素、模态浮层铺满视口——组件级单测证明不了，只有真实控制通道往返能证明 | `py -3 tools/st_editor_smoke.py [gbcode\|gallery\|all]` |
| `gallery_declarative_e2e.py` | gallery | 「声明式」页：`mount_into` 锚点语义/状态驱动传播/key 对齐列表/异步 resource（⚠ 默认取 `build/debug` 档二进制，非 dev 档） | `py -3 tools/gallery_declarative_e2e.py` |
| `gbcode_e2e.py` | gbcode | **主全量走查，29 组**：五层骨架/多标签/底部面板互切/编辑撤销/查找替换/搜索/菜单下拉/命令面板/分栏/主题 + 崩溃栈符号化（需 `crash_probe`，未构建则该步自动跳过） | `py -3 tools/gbcode_e2e.py` |
| `gbcode_e2e_session.py` | gbcode | 会话恢复：MRU → 关窗保存 → 重启恢复（工作区/标签/活动项/光标）→ 菜单与命令面板的「最近」入口 | `py -3 tools/gbcode_e2e_session.py` |
| `gbcode_compare_e2e.py` | gbcode | 对比链路：比较视图 → 文件清单 → 并列差异 → 导航（自带临时真 git 仓库，用完即删） | `py -3 tools/gbcode_compare_e2e.py` |
| `gbcode_git_e2e.py` | gbcode | Git 写面与五个回顾视图：提交面板/危险操作必须确认/冲突给中止入口（自带真仓库 + 真 merge 冲突） | `py -3 tools/gbcode_git_e2e.py` |
| `gbcode_history_e2e.py` | gbcode | 单文件历史弹窗：真仓库 `git log --follow` → 弹窗列出提交 → 点开差异 | `py -3 tools/gbcode_history_e2e.py` |
| `gbcode_blame_e2e.py` | gbcode | 行尾 blame 注解：真仓库 → 数据面 → 编辑器行装饰（断注解真进了渲染，不是只看 setter 被调） | `py -3 tools/gbcode_blame_e2e.py` |
| `gbcode_hint_e2e.py` | gbcode | 悬停提示五断言：延时内不出现/超时出现/移开消失/点击消失/悬停子元素冒泡拿父提示 | `py -3 tools/gbcode_hint_e2e.py` |
| `gbcode_focus_ring_e2e.py` | gbcode | 焦点环像素断言：点击不留环、键盘导航仍画环（同视图状态区域像素对照） | `py -3 tools/gbcode_focus_ring_e2e.py` |
| `gbcode_sidebar_keepalive_e2e.py` | gbcode | 侧栏「常驻声明 + 隐藏保活」：切换后滚动偏移/输入文本不丢 + 帧耗时量级断言 | `py -3 tools/gbcode_sidebar_keepalive_e2e.py` |
| `gbcode_sidebar_keepalive_bench.py` | gbcode | （bench 对照，非断言）常驻声明 vs 旧写法切换的帧耗时分段对照，不改任何代码 | `py -3 tools/gbcode_sidebar_keepalive_bench.py` |
| `gbcode_terminal_keepalive_e2e.py` | gbcode | 终端保活：面板收起再展开，内容/尺寸/会话/工作目录原样（PTY 未被 resize、缓冲未清） | `py -3 tools/gbcode_terminal_keepalive_e2e.py` |
| `gbcode_terminal_scroll_e2e.py` | gbcode | 终端滚动：PTY 尺寸跟随（面板压矮 → 屏幕行数变少）/滚轮回看/拖到底/回到底部，收起展开后仍可用 | `py -3 tools/gbcode_terminal_scroll_e2e.py` |
| `louyue_e2e.py` | louyue | 设计文档 §7 四条验收：两笔不同混合模式笔画导出后独立解码 PNG 逐像素断色（±2/255）/撤销重做逐像素一致/九种混合模式对 W3C 公式/交互链路 | `py -3 tools/louyue_e2e.py` |
| `caiyun_e2e.py` | caiyun | 设计文档 §7 五条验收：帧哈希确定性（同刻两次一致、异刻不同——证明预览跟着播放头走）/修剪分割移动入出点/导出帧数与抽查哈希/`--bench` 压测 | `py -3 tools/caiyun_e2e.py` |
| `xmlapp_e2e.py` | xmlapp | 通路的三个接缝：XML 建出界面树/JS 调 C++ 真落数据/JS 动态建删结构 | `py -3 tools/xmlapp_e2e.py` |

## 一键全跑

```bash
py -3 tools/examples_e2e_all.py              # 上表串行全跑，退出码 = 失败数
py -3 tools/examples_e2e_all.py --with-bench # 追加 bench 对照（无断言）
py -3 tools/examples_e2e_all.py --with-debug # 追加 gallery_declarative（需 debug 档）
```

串行不并行（性能敏感断言怕抢载）；单脚本失败不中断，末尾汇总；
脚本内 SEQUENCE 与本表对齐，清单外的新 `*_e2e.py` 会被对账自检点名——
**新增脚本时同步改本表与 SEQUENCE（或入 KNOWN_EXCLUDED 并注明口径）**。

## 范围口径

只收「驱动示例二进制、走控制通道、带断言」的端到端脚本，两类不收：

- **框架级验证**：`terminal_*_reverse_verify.py`、`x11_win_check.py`、`wayland_win_check.py`
  等——它们驱动 `st`/测试目标，不属某一示例；
- **单点探针与量测**：`*_probe.py`、`*_shot.py` 等无断言或只取数不判定的脚本
  （`gbcode_sidebar_keepalive_bench.py` 因与 keepalive e2e 同源对照而收录并标注）。
