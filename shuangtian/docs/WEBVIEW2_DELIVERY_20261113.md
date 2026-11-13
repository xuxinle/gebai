# WebView2 可编程嵌入组件 — 交付状态

> 本文件是**跨轮次的状态记录**（会话会过去，结论要留在盘上）。
> 写于 2026-11-13，执行「WebView2 可编程嵌入组件：URL 导航 + 资源内嵌」计划之后。

## 一、交付内容（全部已实测验证）

| 层 | 文件 | 状态 |
|---|---|---|
| 第三方 | `third_party/webview2/{WebView2.h, WebView2Loader.dll, LICENSE.txt, NOTICE.txt}` + `SOURCES.md`/`CHECKSUMS.sha256` 登记 | ✅ |
| 平台中立接口 | `include/st/shell/webview.hpp`（`WebViewHost` + `WebAssets` + `State` + `Event`） | ✅ |
| Win32 实现 | `src/shell/platform_win32_webview.cpp`（运行时 Loader、COM RAII、虚拟 origin、DPI 重下发） | ✅ |
| 中立工具 | `src/shell/webview.cpp`（`WebAssets` 的路径归一化/MIME 推断） | ✅ |
| ui 侧端口 | `include/st/ui/webview_port.hpp`（`WebViewPort`/`WebViewState`/`WebViewAsset`/`WebViewEvent`） | ✅ |
| 组件 | `include/st/ui/components/webview.hpp` + `src/ui/components/webview.cpp` | ✅ |
| 折接层 | `src/app/app.cpp` 的 `WebViewPortAdapter` + `Application::webview_*` | ✅ |
| 宿主接线 | `HostFocus::host_webview_port` → `UiRoot::set_webview_port` → `UiRoot::dispatch_webview_events` | ✅ |
| 演示页 | `apps/gallery/webview_page.{hpp,cpp}`（第 8 页，`kPageCount` 8→9） | ✅ |
| 测试 | `tests/ui_webview_test.cpp`（15 用例 / 100 断言） | ✅ |
| 文档 | `DESIGN.md` §4.6.2、`docs/PITFALLS.md` 场景 28 续、`docs/BACKLOG.md` P2 两条、`agent-docs/components/layout/WebView.md`（生成） | ✅ |

## 二、真窗口实测结果（`shuangtian_run action=start window=true`，后端 win32）

| 能力 | 实测值 |
|---|---|
| 宿主创建 | `attached=true`、`supported=true`、`rasterization_scale=1.500` |
| 资源内嵌（虚拟 origin） | `ready=true`、`url=https://app.st.invalid/index.html`、`title=内嵌资源演示`、`assets=4`、`source=embedded` |
| 页内自检（页面自己报的 DOM 读数） | 相对路径 CSS ✓、同源 `fetch` 拿到 `[1,2,3]` ✓、ES module ✓ |
| URL 导航 | `url=https://example.com/`、`title=Example Domain`、`can_back=true`；**DPI 仍为 1.500**（未被导航重置，重下发生效） |
| 后退 | 回到 `内嵌资源演示`，`can_forward=true` |
| 双向消息 | 页面侧读到 `收到宿主消息：hello-from-host` |
| 切页自动隐藏 | `effective_visible=false`、`shown=false`、`attached/ready` 保持；切回自动恢复 |

## 三、门禁结果

- `st check`：**通过**（378 单元）
- `st build gallery --profile dev`：**通过**
- `st test webview --profile debug`：**15 passed / 0 failed / 100 assertions**
- `st lint`：我的文件 **0 项**；剩 5 项是仓库既有（`apps/xiangqi/main.cpp` 的 `detach`、`component_registry.hpp` 的 4 个宏，两文件本次未触碰）
- 逆向验证：做了两次（回退 `ensure_page` 的支持性检查 / 回退桩的入口校验），**都如期变红**，恢复后全绿
- `agent_docs_gen.py --check`：一致（82 页）；`agent_docs_normalize.py --check`：引用全可解析

## 四、已修的两处真实缺陷（都是本次引入的）

1. **端口存在 ≠ 端口支持**：`ensure_page` 原为 `void`，于是"没装 Runtime"时仍会发导航请求（看着在做事、实际不会发生）。改为 `[[nodiscard]] bool` + 调用方提前返回。
2. **独立工程链接失败**：加了 `ole32`/`uuid`/`shlwapi` 只改了 `link-libs.txt`，漏了 `src/pkg/build.cpp` 的 `kFallbackLibsWindows` ⇒ `independent_project_builds_links_and_runs` 红在 `undefined symbol: CoTaskMemFree`。已同步并写进 PITFALLS。

## 五、未完成 / 遗留

- ⚠ **`terminal_pty_stop_interrupts_without_killing_the_shell` 红，但与本改动无关**：
  用「stash 掉全部改动后重建的基线二进制」跑同一用例，**同样红**（三条断言完全相同）。
  机器负载 0、其余 6 个 PTY 用例全绿，属既有问题（该用例历史注释已写明它负载敏感）。
  未修——不在本次范围内，需要单独一轮。
- ⚠ `docs/PITFALLS.md` 里 `src/ui/components/webview.cpp` 的引用在 `check_docs` 里被标
  「存在但未入仓」——**入仓（`git add`）后即消失**。本次未提交，故仍会报。
- ⚠ `check_docs` 另报 12 项既有引用问题（`window_chrome.hpp`、`three_parity.cpp` 等），
  均为仓库存量，与本次无关。
- `b::embed` 无法枚举 ⇒ 内嵌资源只能逐条注册；同窗口只支持一个页面对象。
  两条都已进 `docs/BACKLOG.md` P2。

## 六、复现命令

```powershell
cd shuangtian
.\build\bin\st.exe check
.\build\bin\st.exe build gallery --profile dev
.\build\bin\st.exe test webview --profile debug
```

真窗口验证（需 `shuangtian` 子代理）：

```
shuangtian_run action=start framework=<repo>/shuangtian target=gallery window=true
shuangtian_invoke id=nav-webview action=click
shuangtian_invoke id=btn-wv-embedded action=click
shuangtian_get id=webview-demo        # 看 ready / url / title / rasterization_scale
shuangtian_invoke id=webview-demo action=exec argument="document.title"
```
