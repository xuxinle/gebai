/** 后台会话连续性（前端断开不停任务 / 随时重连恢复状态）的链路守卫。
 *  这些接线分散在多个模块、彼此隐式依赖，缺一根都不会触发 typecheck/lint（表现为：任务被看门狗误杀、
 *  待决卡片不重建、界面谎报空闲），故以源码扫描钉住关键接线（与 attach.test.ts 同一约定）。 */
import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const src = (file: string) => readFileSync(join(import.meta.dirname, file), "utf8")

describe("看门狗不得误杀后台任务", () => {
  test("断线期间、页面不可见期间、以及「等待用户作答」期间都不取消任务", () => {
    const s = src("stream.ts")
    expect(s).toContain("if (isPageHidden()) return")
    expect(s).toContain("if (!client.isConnected()) return")
    expect(s).toContain("if (hasPendingInteraction(sessionId)) return")
    // 取消只在「可见 + 已连接 + 无待决交互 + 有效空闲超时」时才发出
    const idle = s.slice(s.indexOf("const idleTimer = setInterval"), s.indexOf("const idleTimer = setInterval") + 900)
    expect(idle.indexOf("isPageHidden()")).toBeLessThan(idle.indexOf("client.cancelTask"))
    expect(idle.indexOf("!client.isConnected()")).toBeLessThan(idle.indexOf("client.cancelTask"))
    expect(idle.indexOf("hasPendingInteraction(sessionId)")).toBeLessThan(idle.indexOf("client.cancelTask"))
  })

  test("后台时长不计入空闲：看门狗按有效空闲（扣除隐藏时段）判定，不用墙钟间隔", () => {
    const s = src("stream.ts")
    const idle = s.slice(s.indexOf("const idleTimer = setInterval"), s.indexOf("const idleTimer = setInterval") + 900)
    expect(idle).toContain("effectiveIdleMs(run.lastActivity)")
    // 墙钟直接相减会把整段后台时间当成无数据挂起（手机切 App 即中招）
    expect(idle).not.toContain("Date.now() - run.lastActivity")
    // 隐藏时段登记必须绑定（否则 isPageHidden/effectiveIdleMs 永远是「从未隐藏」）
    expect(src("stream.ts")).toContain("initPageIdle()")
  })

  test("交互等待心跳与工具心跳同等刷新活跃时间（服务端等待期无其他数据）", () => {
    const s = src("main.ts")
    expect(s).toContain('ev.type === "event.tool.alive" || ev.type === "event.interaction.alive"')
  })

  test("附加失败退避重试（静默失败会让界面谎报空闲）", () => {
    const s = src("attach.ts")
    expect(s).toContain("scheduleAttachRetry")
    expect(/if \(!attached\) scheduleAttachRetry\(sessionId\)/.test(s)).toBe(true)
  })
})

describe("缺口（overrun）后的全量重同步闭环", () => {
  test("resume 携带 reloadHistory：重读消息列表 + 重建待决交互卡 + 重载期间暂停正文渲染", () => {
    const s = src("stream.ts")
    expect(s).toContain("chunk.reloadHistory")
    expect(s).toContain("await loadMessages(sessionId)")
    expect(s).toContain("await resyncPendingInteractions(sessionId)")
    expect(s).toContain("run.reloading = true")
    expect(s).toContain("rehydrateRunView(run, sessionId)")
    expect(s).toContain("if (run.reloading) return") // 重载期间只累积，由 rehydrateRunView 重建气泡
  })

  test("待决卡片重建与 attach 路径共用同一分派（不漏任何一类交互）", () => {
    const shared = src("pending-interactions.ts")
    for (const kind of ["approval", "choice", "env", "draw", "capture"]) {
      expect(shared).toContain(`\"${kind}\"`)
    }
    expect(src("attach.ts")).toContain('from "./pending-interactions"')
    expect(src("stream.ts")).toContain('from "./pending-interactions"')
  })

  test("在途工具卡重建：attach 快照的 tools 走实时事件同一渲染入口（结果到达即配对填充）", () => {
    const shared = src("pending-interactions.ts")
    expect(shared).toContain("export function renderPendingToolCalls")
    expect(shared).toContain("onToolCall({ sessionId, toolCallId: t.toolCallId")
    expect(shared).toContain("renderPendingToolCalls(sessionId, snap?.tools)")
    expect(src("attach.ts")).toContain("renderPendingToolCalls(sessionId, snap.tools)")
  })
})

describe("运行态可见性与列表角标", () => {
  test("快照 runtime 进入前端状态并驱动列表角标", () => {
    expect(src("main.ts")).toContain("setRuntimeInfo(snap.runtime ?? {})")
    const s = src("sessions.ts")
    expect(s).toContain("export function setRuntimeInfo")
    expect(s).toContain("export function markSessionRunning")
    expect(s).toContain("session-run") // 行内角标元素
    expect(s).toContain("signatureOfRun(runMarkOf(s.id))") // 运行态变化触发列表重建
  })

  test("任务开始/结束事件即时收敛角标（不必等下一次快照）", () => {
    const s = src("main.ts")
    expect(s).toContain("markSessionRunning(ev.sessionId, true)")
    expect(s).toContain("markSessionRunning(ev.sessionId, false)")
  })
})

describe("断线/失效期间的用户动作不静默丢", () => {
  test("审批提交失败保留卡片并提示；服务端判失效则撕卡说明", () => {
    const s = src("approvals.ts")
    expect(s).toContain("await client.decideApproval")
    expect(s).toContain('=== "expired"')
    expect(s).toContain("export function hasPendingInteraction")
  })

  test("选择/填值卡的失效与提交失败同样如实反馈", () => {
    const s = src("tool-cards.ts")
    expect(s).toContain("该询问已失效")
    expect(s).toContain("该填值请求已失效")
    expect(s).toContain("await client.decideEnv(sessionId, envId, null)") // 拒绝也要送到
  })

  test("停止未送达如实提示；已有任务在跑时不静默吞消息（撤回幻影消息 + 保住输入）", () => {
    expect(src("composer.ts")).toContain("停止未送达")
    const s = src("main.ts")
    expect(s).toContain('code === "already_running"')
    // 幻影消息必须被撕掉（先回收附件对象 URL 再移除元素，不然 blob 留在 URL store 里）
    const branch = s.slice(s.indexOf('code === "already_running"'), s.indexOf('code === "already_running"') + 600)
    expect(branch).toContain("releaseMsgAttachments(userMsgEl)")
    expect(branch).toContain("userMsgEl.remove()")
    expect(s).toContain("input.value = text") // 内容放回输入框
  })
})

describe("附图消息发送即上屏（不等上传回流）", () => {
  test("main.ts 先上屏（含本地附件预览）再上传，完成后原地换为服务端引用", () => {
    const s = src("main.ts")
    const onScreen = s.indexOf("renderMsgAttachments(userMsgEl, sessionId, localFiles)")
    // 直接发送路径的上传点（运行中分支另有一个 sendPending，取最后一次出现）
    const upload = s.lastIndexOf("await sendPending(sessionId)")
    const replace = s.indexOf("renderMsgAttachments(userMsgEl, sessionId, attachments)")
    expect(onScreen).toBeGreaterThan(-1)
    // 上屏必须在上传之前（反了则附图消息“先发后消失”、纯附图消息完全不可见）
    expect(onScreen).toBeLessThan(upload)
    // 上传完成后原地重渲染（不重建消息，历史渲染同一入口）
    expect(replace).toBeGreaterThan(upload)
    expect(s).toContain("releaseMsgAttachments") // 幻影消息撕掉前回收对象 URL
  })

  test("队列消化（背景会话）的用户消息同样带附件引用渲染", () => {
    const s = src("main.ts")
    const at = s.lastIndexOf("setQueueExecutor(") // 模块尾部才是注册点（顶部是同名 import）
    const fn = s.slice(at, at + 900)
    expect(fn).toContain("attachments: item.files")
  })
})

describe("后台会话 DOM 维护与无缝切换", () => {
  test("后台工具事件渲染到该会话自己的容器（不再直接丢弃）", () => {
    const s = src("events.ts")
    // 三态判定：有 pane 渲染、当前会话无 pane 也渲染、其余不渲染
    expect(s).toContain("paneForRender")
    expect(s).toContain("withPane(pane ?? null")
    // 旧行为（非当前会话一律 return）不得回归：非当前会话不再无条件丢弃
    for (const fn of ["onToolCall", "onToolResult"]) {
      const at = s.indexOf(`export function ${fn}`)
      const body = s.slice(at, at + 400)
      expect(body).not.toContain("if (ev.sessionId !== cur?.id) return")
    }
  })

  test("运行开始即建容器并标记运行态（后台也是）；结束降为可回收", () => {
    const s = src("stream.ts")
    expect(s).toContain("ensurePane(sessionId)")
    expect(s).toContain("setPaneRunning(sessionId, true)")
    expect(s).toContain("setPaneRunning(sessionId, false)")
    // 正文/推理渲染按「有无容器」而非「是否当前会话」判定（否则后台不建 DOM）
    expect(s).toContain("if (!paneOf(sessionId)) return")
  })

  test("切换会话走快速路径（纯显隐 + 块表/导航恢复 + 滚动位，不重拉消息）", () => {
    const s = src("sessions.ts")
    expect(s).toContain("activatePaneFast")
    const at = s.indexOf("async function activatePaneFast")
    // 函数体已变长（块表恢复/导航/刷新子会话等）：切片取到下一个函数定义为止，
    // 不用固定长度——否则新增几行就把后面的断言截掉（假阴性）
    const end = s.indexOf("/** 切换会话（列表委托分发", at)
    const fn = s.slice(at, end > at ? end : at + 3000)
    expect(fn).toContain("activatePane(sid)")
    expect(fn).not.toContain("loadMessages(") // 快速路径绝不重拉
    // 缓存页切回必须重立自己的导航段表（否则导航列停在上一个会话）
    expect(fn).toContain("buildMsgNav(t.msgs)")
    // 切换入口：先试快速路径，不成才走常规加载
    const act = s.slice(s.indexOf("async function activateSession"), s.indexOf("async function activateSession") + 1600)
    expect(act).toContain("if (await activatePaneFast(sid))")
  })

  test("随会话变化的 UI 收在单一入口 syncSessionScopedUi（两条切换路径都走它）", () => {
    const s = src("sessions.ts")
    // 入口定义：清单里的每一项都必须在这个函数里（而不是散落各路径）
    const at = s.indexOf("function syncSessionScopedUi(")
    expect(at).toBeGreaterThan(-1)
    const end = s.indexOf("\nexport async function loadMessages", at)
    const hub = s.slice(at, end > at ? end : at + 1200)
    for (const item of [
      "syncActiveRow(", // 会话列表高亮
      "updateTitle(", // 标题
      "syncSendButton(", // 发送/停止
      "renderQueue(", // 排队条
      "applyApprovalVisibility(", // 待审批卡
      "applyInteractionVisibility(", // 待作答交互卡
      "updateMsgNav(", // 消息导航列
      "refreshSubSessions(", // 子会话按钮与面板
    ]) {
      expect(hub).toContain(item)
    }

    // 两条切换路径 + 草稿页都必须调入口
    const fast = s.slice(s.indexOf("async function activatePaneFast"), s.indexOf("/** 切换会话（列表委托分发"))
    expect(fast).toContain("syncSessionScopedUi()")
    const full = s.slice(s.indexOf("export async function loadMessages"), s.indexOf("export async function loadMessages") + 9000)
    expect(full).toContain("syncSessionScopedUi()")
    const draft = s.slice(s.indexOf("export function enterDraftView"), s.indexOf("export function enterDraftView") + 900)
    expect(draft).toContain("syncSessionScopedUi()")
  })

  test("空白页在无 pane 时挂到宿主（否则「歌未竟 东方白」不显示）", () => {
    const s = src("sessions.ts")
    // 消息列按会话分容器后，草稿页没有任何 pane；直接 appendTail 会被转发到空 pane 而丢弃
    const at = s.indexOf("function mountEmptyState")
    expect(at).toBeGreaterThan(-1)
    const fn = s.slice(at, at + 400)
    expect(fn).toContain("activePane()")
    expect(fn).toContain("msgEl.appendChild(node)") // 回退到宿主
    // 空状态必须单例复用：反复进出草稿页不能堆出多份
    expect(s).toContain("function emptyStateNode()")
    expect(s).toContain("if (emptyStateEl) return emptyStateEl")
    // 有内容的会话必须清掉宿主上的空白页（否则与 pane 并存）
    const hub = s.slice(s.indexOf("function syncSessionScopedUi("), s.indexOf("export async function loadMessages"))
    expect(hub).toContain("hideEmptyState()")
    // 判据取容器里有无消息节点（不读 chunks：快速路径下可能未就绪）
    expect(hub).toContain('querySelector(".msg")')
  })

  test("随会话变化的 UI 不得在切换路径里散落手写（漏项根因）", () => {
    const s = src("sessions.ts")
    const fast = s.slice(s.indexOf("async function activatePaneFast"), s.indexOf("/** 切换会话（列表委托分发"))
    // 快速路径里除统一入口外不应再单独刷这些（各写一份就是漏项的根源）
    for (const item of ["syncSendButton()", "renderQueue()", "refreshSubSessions()", "syncActiveRow("]) {
      expect(fast.split(item).length - 1).toBe(0)
    }
  })

  test("切回会话定位前按意图取窗（贴底不看缓存页残留 scrollTop）", () => {
    const v = src("virtualize.ts")
    // 贴底时窗口从末尾往回取，不依赖当前 scrollTop（缓存页的 scrollTop 是上次离开的值）
    expect(v).toContain("function bottomRange(")
    const sync = v.slice(v.indexOf("function sync()"), v.indexOf("function sync()") + 1400)
    expect(sync).toContain("keepBottom ? bottomRange(viewportH)")

    const s = src("sessions.ts")
    const at = s.indexOf("async function activatePaneFast")
    const fn = s.slice(at, at + 2200)
    // 先声明「追最新」意图再 sync（sync 内部按意图取窗并钉底），且只定位一轮——
    // 反复写 scrollTop 就是切回时拧巴着抖的直接原因
    expect(fn).toContain("lockToBottom()")
    expect(fn.indexOf("lockToBottom()")).toBeLessThan(fn.indexOf("pane.virtualizer.sync()"))
    expect(fn.split("lockToBottom()").length - 1).toBe(1)
  })

  test("回收必须在切换之后（切前回收会把目标容器一并收掉，快速路径永不命中）", () => {
    const s = src("sessions.ts")
    const at = s.indexOf("async function activateSession")
    const act = s.slice(at, s.indexOf("/** 触屏长按判定阈值", at))
    const fast = act.indexOf("activatePaneFast(sid)")
    const reap = act.indexOf("reapIdlePanes()")
    expect(fast).toBeGreaterThan(-1)
    expect(reap).toBeGreaterThan(fast) // 首次回收只能出现在切换之后
    // 切前不得回收（旧缺陷：reap 在 setCurrentSession 之前，把目标容器也收掉）
    expect(act.slice(0, fast)).not.toContain("reapIdlePanes()")
  })

  test("已停止会话按 LRU 缓存（不是切走即释放）：来回切换不重拉", () => {
    const s = src("msg-panes.ts")
    expect(s).toContain("IDLE_PANE_CACHE")
    const fn = s.slice(s.indexOf("export function reapIdlePanes"), s.indexOf("export function reapIdlePanes") + 800)
    // 按 lastUsed 排序取最久未用的释放（LRU），而不是全部清掉
    expect(fn).toContain("lastUsed")
    expect(fn).toContain("slice(0, excess)")
  })

  test("隐藏容器保持窗口化（不因 clientHeight=0 退化为全量渲染）", () => {
    const s = src("virtualize.ts")
    // 视口高度走「最近可测值 + 兜底」，不能把 0 直接交给区间计算
    expect(s).toContain("function viewportHeight()")
    expect(s).toContain("FALLBACK_VIEWPORT_H")
    expect(s).toContain("lastViewportH")
    const sync = s.slice(s.indexOf("function sync()"), s.indexOf("function sync()") + 900)
    expect(sync).toContain("viewportHeight()")
    expect(sync).not.toContain("const viewportH = container.clientHeight") // 旧写法：隐藏即 0 → 全量
  })

  test("容器自带块表（缓存页切回靠它重建历史块，不靠全局单例）", () => {
    expect(src("msg-panes.ts")).toContain("chunks: PaneChunks | null")
    const s = src("sessions.ts")
    expect(s).toContain("function chunksOf(")
    expect(s).toContain("pane?.chunks")
  })

  test("非活动容器不得驱动全局「回到最新」按钮", () => {
    expect(src("sticky-scroll.ts")).toContain("isActive")
    expect(src("msg-panes.ts")).toContain("isActive: () =>")
  })

  test("几何单例按会话实例化：窗口化/粘底/导航都转发到活动 pane", () => {
    expect(src("msg-window.ts")).toContain("activePane()?.virtualizer")
    expect(src("jump-bottom.ts")).toContain("current?.sticky")
    expect(src("msg-nav.ts")).toContain("activePane()?.container ?? msgEl")
    // 导航的滚动/尺寸/内容三类监听必须可重绑（切会话即换容器）
    expect(src("msg-nav.ts")).toContain("onPaneActivate")
  })
})

describe("会话列表刷新不得回退数据", () => {
  test("任务开始/结束的列表刷新拉新数据，不用 lastSessions 陈旧数组重流（否则自动命名的新标题被 seq 守卫丢弃）", () => {
    const s = src("sessions.ts")
    const fn = s.slice(s.indexOf("export function markSessionRunning"), s.indexOf("export function markSessionRunning") + 800)
    // 刷新必须是不带 preloaded 的拉新调用；带 lastSessions 的旧写法会把在途自动命名刷新挤掉
    expect(fn).toContain("void refreshSessions()")
    expect(fn).not.toContain("refreshSessions(lastSessions)")
  })

  test("自动命名 rename 后必发一次全量刷新（与任务结束刷新并发时 seq 守卫保最后完成者赢）", () => {
    const s = src("sessions.ts")
    const fn = s.slice(s.indexOf("export async function maybeAutoTitle"), s.indexOf("export async function maybeAutoTitle") + 1600)
    expect(fn).toContain("await client.renameSession(sessionId, title)")
    expect(fn.indexOf("await client.renameSession(sessionId, title)")).toBeLessThan(fn.indexOf("await refreshSessions()"))
  })
})
