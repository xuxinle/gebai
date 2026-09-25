# 端到端（e2e）验证脚本

浏览器里的真实行为验证——单测覆盖不到的层（布局度量、事件时序、真实浏览器与真实 PTY 的交互）
都在这里跑。脚本自成一体（Playwright + Chromium），**不纳入 `bun test`**：它们需要一份正在运行的
服务端与真实终端会话，属于「改完手动跑一遍」的验证工具。

## 前置

```bash
# ① 依赖（仓库根已声明 playwright）
bun install
bunx playwright install chromium   # 首次

# ② 起服务（任选其一）
bun run dev                        # 开发模式，默认 http://localhost:3000
# 或指向任意已部署实例：脚本第一个参数就是 baseUrl
```

## 用法

```bash
bun run e2e:term          # 终端面板：31 项（尺寸/折行/配色/搜索/键位/标签/粘贴/关闭确认/刷新接管）
bun run e2e:term:tui      # 独占模式：23 项（vi / less / watch / htop：备用屏、SIGWINCH、鼠标上报、搜索高亮）
bun run e2e:term:edit     # 行内编辑：30 项（光标移动与前删/后删、宽字符、折行、输入法整段提交、vim）
bun run e2e:term:all      # 三个依次跑

# 自定义地址与失败现场截图目录
bun run scripts/e2e/files-e2e.mjs http://127.0.0.1:5174
E2E_ARTIFACTS=/tmp/mine bun run e2e:term
```

失败时：退出码非 0，并打印未通过项；现场截图落在 `E2E_ARTIFACTS`（缺省 `/tmp/gebai-e2e`）。

## 三条脚本约定（踩过的坑，改脚本时别丢）

1. **开头先回收残留的 PTY 会话**：服务端并发上限 8，上一次异常退出留下的会话会占满额度，
   之后所有「新建终端」都会失败——看上去像大面积回归，其实只是没额度。
2. **终端内容用「有内容的行」判断**：终端只有几十行、底部是空行，取 `slice(-1)` 会取到空行；
   折行输出要拼行（`screenJoined`）后再比对。
3. **同一个标记行可能出现在多处**（连续两段 `cat …; echo MARK`）：从底部取最近的一次，
   否则会读到上一段的内容。
4. **不要用 `.xterm-viewport` 的 `scrollTop/scrollHeight` 判断滚动**：xterm 6 的滚动条是自绘的，
   两者恒等——这种断言永远通过（空断言）。要看**可见首行**。
