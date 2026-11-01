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
| `lsp_symbols_probe`（C++ 探针） | gbcode | LSP 链路分段验证：握手耗时 / **didOpen 前后 `documentSymbol` 的差别（0 vs 16 个符号）**——大纲恒空的根因探针，改 LSP 桥时先跑它 | `st build lsp_symbols_probe && ./build/dev/bin/lsp_symbols_probe <file>` |
| `gbcode_focus_ring_e2e.py` | gbcode | 焦点环像素断言：点击不留环、键盘导航仍画环（同视图状态区域像素对照） | `py -3 tools/gbcode_focus_ring_e2e.py` |
| `gbcode_sidebar_keepalive_e2e.py` | gbcode | 侧栏「常驻声明 + 隐藏保活」：切换后滚动偏移/输入文本不丢 + 帧耗时量级断言 | `py -3 tools/gbcode_sidebar_keepalive_e2e.py` |
| `gbcode_sidebar_keepalive_bench.py` | gbcode | （bench 对照，非断言）常驻声明 vs 旧写法切换的帧耗时分段对照，不改任何代码 | `py -3 tools/gbcode_sidebar_keepalive_bench.py` |
| `gbcode_terminal_keepalive_e2e.py` | gbcode | 终端保活：面板收起再展开，内容/尺寸/会话/工作目录原样（PTY 未被 resize、缓冲未清） | `py -3 tools/gbcode_terminal_keepalive_e2e.py` |
| `gbcode_terminal_scroll_e2e.py` | gbcode | 终端滚动：PTY 尺寸跟随（面板压矮 → 屏幕行数变少）/滚轮回看/拖到底/回到底部，收起展开后仍可用 | `py -3 tools/gbcode_terminal_scroll_e2e.py` |
| `gbcode_usability_e2e.py` | gbcode | **真实使用者路径**（真磁盘工作区 + 真 git 仓 + 真 shell）：树行可读与零坐标驱动/BOM 与 CRLF 保真/Shift+Tab 反缩进/查找替换/终端忙闲语义与标签短名/菜单项按 id 激活/状态栏格子反馈/Git 暂存与提交真落盘/大文件滚动 | `py -3 tools/gbcode_usability_e2e.py` |
| `louyue_e2e.py` | louyue | 设计文档 §7 四条验收：两笔不同混合模式笔画导出后独立解码 PNG 逐像素断色（±2/255）/撤销重做逐像素一致/九种混合模式对 W3C 公式/交互链路 | `py -3 tools/louyue_e2e.py` |
| `caiyun_e2e.py` | caiyun | 设计文档 §7 五条验收：帧哈希确定性（同刻两次一致、异刻不同——证明预览跟着播放头走）/修剪分割移动入出点/导出帧数与抽查哈希/`--bench` 压测 | `py -3 tools/caiyun_e2e.py` |
| `xmlapp_e2e.py` | xmlapp | 通路的三个接缝：XML 建出界面树/JS 调 C++ 真落数据/JS 动态建删结构 | `py -3 tools/xmlapp_e2e.py` |
| `hot_reload_e2e.py` | xmlapp | **改文件即生效**（真进程）：改 XML 换树 / 改 JS 重装且绑定不叠 / 写错 XML 不清界面 / 写错 JS 不丢逻辑 | `py -3 tools/hot_reload_e2e.py` |
| `gedou_e2e.py` | gedou | **类拳皇对战**的 7 组断言：判定只在判定帧出盒 / 命中扣血 = 招式表 / 判定盒在朝向方向 / 必杀破防且击倒 / 血空→over+胜者胜利姿态+KO / 打开判定盒画面真变（像素哈希 + 亮度统计）/ AI 同种子两跑逐值一致 | `py -3 tools/gedou_e2e.py` |
| `gedou_reverse_verify.py` | gedou | **逆向验证**（非普通断言）：逐条把修复回退→确认重编成功→确 e2e/单测变红→恢复回绿。7 个案例（含一个"应当保持绿"的对照：期望值与实现同源） | `py -3 tools/gedou_reverse_verify.py [--only 案例名]` |
| `gedou_balance_check.py` | gedou | **训练前的平衡体检**：8 个固定动作 × 3 档难度矩阵。判据写死：任一固定动作胜率 ≥ 90% 即"无脑必胜"，训练没意义（实测：不体检直接训，"一直按踢"100% 胜） | `py -3 tools/gedou_balance_check.py --episodes 80` |
| `gedou_rl_train.py` | gedou | **PPO 训练**：collect→train→eval 循环，每次导出自动跑前向自检（torch vs C++ 逐值），输出权重 JSON + 报告（含随机/手写两条基线） | `py -3 tools/gedou_rl_train.py --rounds 26 --episodes 320 --eval-episodes 240 --entropy 0.05` |
| `gedou_rl_plot.py` | gedou | 把训练报告画成曲线（胜率/熵/回报三联，基线水平线并排） | `py -3 tools/gedou_rl_plot.py` |
| `gedou_rl_reverse_verify.py` | gedou | RL 链路的**逆向验证**：前向矩阵/自我中心观测/奖励符号/冷却/观测维度漂移，五个回退各自必须让对应用例变红 | `py -3 tools/gedou_rl_reverse_verify.py [--only 案例名]` |
| `gui_subsystem_check.py` | 全部桌面应用 | **读产物**而非读构建输出：PE 头的 Subsystem 是否 gui(2)、exe 里有没有 256 图标；命令行工具是否仍保持 console(3)。带 `--reverse` 逐项回退验证 | `py -3 tools/gui_subsystem_check.py [--reverse]` |

## 桌面应用：图标与子系统

每个桌面应用都有**两样**交付属性，二者都写在 `st.pkg` 的 target 里：

```json
"gallery": { ..., "system": "gui", "icon": "examples/gallery/assets/gallery.ico" }
```

| 属性 | 效果 | 没配的后果 |
|---|---|---|
| `system: "gui"` | PE Subsystem=2，双击不弹黑框 | 双击先弹一个空控制台窗口 |
| `icon` | exe **文件**图标（资源管理器/任务栏/快捷方式） | 显示成默认空白图标 |

> `icon` 只影响磁盘上的 exe。代码里 `SetWindowIcon` 换的是**窗口**图标（任务栏缩略图等），
> 两者不是一回事。

### 图标从哪来（可重生成）

```bash
py -3 tools/make_icons.py            # 画 PNG 源图（纯 Python，无 Pillow）
py -3 tools/make_ico.py <src.png> <out.ico>   # 转多尺寸 ico（16/24/32/48/64/128/256）
```

源图**用代码画**而不是找素材：配色/圆角/线宽要能批量调，位图做不到。
每个应用一个主题色 + 一个能表意的符号（gallery 九宫格、louyue 版式、
caiyun 胶片+播放、gedou 拳+冲击波、cefu 书+检索、xiangqi 卡片+人像）。

### 验证

```bash
py -3 tools/gui_subsystem_check.py            # 当前产物对不对
py -3 tools/gui_subsystem_check.py --reverse  # 逐项回退→必须变红
```

**这个检查必须读产物，不能读构建输出**：`st build` 说“完成”不代表子系统/图标改过了——
详见下面“链接期输入”。

### 链接期输入（踩过的坑）

`system` 与 `.ico` **不参与单元编译**，只影响链接与资源编译。而增量判定的两道门
最初都没把它们算进去：

1. `link_fingerprint` 只算对象清单 + 编译标志 → 改 `system` 后“链接已跳过”，
   产物仍是 CONSOLE；
2. 图标资源只看 `.rc` **路径**变没变 → 换 `.ico` 内容（路径不变）不重编资源。

两道门现在都按**内容**比（`.ico` 字节哈希），`gui_subsystem_check.py --reverse`
就是用来钉住它们的。

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
