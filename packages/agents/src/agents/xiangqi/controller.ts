/**
 * 相期控制器：**宿主进程级的 WebSocket 服务端**。 *
 * ## 方向：相期是客户端，控制器是服务端
 *
 * ```
 * 相期（桌面窗口）──ws://host:port/xiangqi──▶ 控制器（本文件，跑在 gebai 宿主里）
 *                ◀── {"type":"cards","cards":[…]}     下发卡片（权威列表，整批替换）
 *                ──▶ {"type":"answer","id","kind",…}   用户答复
 * ```
 *
 * 这个方向不是我挑的，是相期那边定的（`examples/xiangqi` 用自研 WS 客户端连出去，
 * 见它的 README）。**把方向搞反的后果很隐蔽**：控制器去主动连 `url` 会连到
 * 自己的服务端形成自环——`isConnected` 报 true、`send` 不报错，
 * 但卡片永远到不了窗口（实测踩到：`#cards` 一直是空的）。
 *
 * ## 为什么不是会话级
 *
 * 相期是常驻窗口。如果连接跟着会话走：
 *
 * 1. 会话一结束连接就断，窗口变孤儿；
 * 2. 多个会话各自监听/连接 → 抢同一个端口或抢同一个窗口，卡片互相覆盖。
 *
 * 所以控制器是**进程级单例**，工具只是往它上面投递。
 *
 * ## 答复的两条通路
 *
 * 相期除了经 WS 回 `answer`，还会把答复落 `outbox/<id>.json`。即使这条 WS 断在
 * 答复那一刻，答复也不会丢——这是 `card_ask` 能安全超时的前提。
 */

import { existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs"
import { join } from "node:path"

/**
 * 相期除了经 WS 回 `answer`，还会把答复**总是**落 `outbox/<id>.json`
 * （不只 WS 失败时）。所以即使这条 WS 断在答复那一刻，答复也不会丢——
 * 这是 `card_ask` 能安全超时的前提，也是本文件里 `outbox` 轮询的存在理由。
 *
 * ## 两条通路（与相期的降级设计对应）
 *
 * | 方向 | 主路 | 降级路 |
 * |---|---|---|
 * | 推卡 | WS `{type:cards}` | 写 `inbox/<name>.json` |
 * | 答复 | WS `{type:answer}` | 读 `outbox/<id>.json` |
 *
 * 为何降级路必要：相期可能刚启动还没连上、或 WS 握手失败，此时写文件仍然
 * 让人能看到提醒（它启动时会 `poll_inbox()` 吃掉已投递的卡片）。反过来，
 * 它点完按钮那一刻 WS 恰好断了，答复已落盘，我们扫 `outbox` 就能捡回来。
 *
 * 注：`inbox/` 只作**兼底**用——WS 通了就以 WS 为准，避免两路同时送
 * 导致同一张卡在窗口上出现两次（相期对 WS 是整批替换，文件是追加）。
 */

/** 一张卡片的三种性质。与相期的 `kind` 一一对应。 */
export type CardKind = "notice" | "choice" | "todo"

/** 投递出去的卡片（对外形态，与相期协议一致）。 */
export interface CardSpec {
  /** 业务侧 id（调用方给；未给时由控制器生成）。 */
  id?: string
  kind: CardKind
  title: string
  /** 正文，**markdown**。相期用 MarkdownView 渲染。 */
  body?: string
  /** 来源标签（谁发的）。 */
  from?: string
  /**
   * `choice` 选填：选项列表。
   *
   * `url` 不为空时该选项渲染为**链接按钮**（点击打开浏览器）——
   * 适用「去授权」「看详情」这类需要用户离开去别处操作的事。
   * 带 url 的选项不会被「选中」，它只负责把用户送到目标页面。
   */
  options?: Array<{ label: string; value: string; hint?: string; url?: string }>
  /** `todo` 可选：截止说明（自由文本，如「今天」「2026-10-25」）。 */
  due?: string
  /**
   * 卡片的新鲜度：`null`/缺省 = 一直显示到用户处理或撤回。
   *
   * 授权类卡片应当带 TTL：链接 10 分钟就过期了，卡片一直挂着只会引导用户点开一个废链接。
   */
  expiresAtMs?: number
}

/** 用户对某张卡的答复。 */
export interface CardAnswer {
  id: string
  kind: CardKind
  /** 各 kind 的答复体：notice→{acknowledged} choice→{choice} todo→{done}|{sn |
   *  snooze} */
  answer: Record<string, unknown>
  answeredAtMs: number
}

/** 已投递、仍在等答复的卡。 */
interface PendingCard {
  spec: CardSpec & { id: string }
  /** 发出时刻（超时计算基准）。 */
  sentAtMs: number
  /** 答复到达时调用。 */
  resolve: (answer: CardAnswer) => void
  /** 取消等待（`close()` 时叫醒，避免吊着）。 */
  cancel: () => void
}

/** 相期那一侧的连接（Bun 的 ServerWebSocket 子集）。 */
export interface PeerSocket {
  send(data: string): void
  close(): void
}

export interface ControllerOptions {
  /** 监听端口。0 = 随机（测试用）；缺省 7717。 */
  port?: number
  /** 监听地址。缺省 `127.0.0.1`（**只本机**：相期是本地窗口，不该暴露到网）。 */
  hostname?: string
  /**
   * 相期的数据根（里面是 `inbox/` / `outbox/`）。
   *
   * 缺省顺序：`XIANGQI_HOME` → `{GEBAI_HOME}/相期` → `<cwd>/相期`。
   * 握手不成时靠这目录做文件兼底。传空串 = **禁用文件通路**（只走 WS）。
   */
  homeDir?: string
  /** outbox 轮询间隔（毫秒）。缺省 500。0 = 不轮询（只靠 WS）。 */
  outboxPollMs?: number
  /** 注入时钟（测试用）。 */
  now?: () => number
}

/** 相期控制器的公开面。 */
export interface XiangqiController {
  /** 实际监听地址（`port=0` 时要读这个才知道端口）。 */
  url(): string
  /** 当前有没有相期连过来（供工具如实汇报，不假装）。 */
  isConnected(): boolean
  /**
   * 断开当前相期连接（仅测试/诊断用）。
   *
   * 用来模拟「WS 断在用户点按钮那一刻」——那正是 `outbox` 兼底
   * 真正救命的场景。不提供这个钩子的话，这个场景只能靠拔网线验证。
   *
   * 生产代码不应调用它（相期会自己重连，调用方无需也不该干预）。
   */
  dropPeerForTest(): void
  /**
   * 启动监听（幂等）。
   *
   * 不调也行——第一次 `push` 会自动起。但**测试与健康检查**需要
   * “先起好再验证”，否则得推一张假卡催启动，而那张假卡会污染卡片列表。
   */
  listen(): void
  /** 投递一张卡；`delivered=false` 表示此刻送不到（卡片已缓存，连上会重推）。 */
  push(spec: CardSpec): { id: string; delivered: boolean }
  /** 等某张卡的答复；超时返回 null。 */
  waitForAnswer(id: string, timeoutMs: number): Promise<CardAnswer | null>
  /** 当前等答复的卡（供 `card_list`）。 */
  pending(): Array<{ id: string; title: string; kind: CardKind; sentAtMs: number }>
  /** 撤回全部卡片。 */
  dismissAll(): void
  /** 撤回单张卡片（授权完成/失效后让那张卡自动消失）；返回是否真的撤了。 */
  dismiss(id: string): boolean
  /** 关掉监听并释放端口（宿主退出 / 测试收尾）。 */
  close(): void
}

const DEFAULT_PORT = 7717
const DEFAULT_HOST = "127.0.0.1"

/** 从环境读端口（`XIANGQI_PORT`）；非法值回落缺省。 */
function portFromEnv(): number {
  const raw = process.env.XIANGQI_PORT
  if (raw === undefined || raw.trim() === "") return DEFAULT_PORT
  const parsed = Number.parseInt(raw, 10)
  // 0 是合法的（让系统挑）；负数/非数字才回退
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : DEFAULT_PORT
}

/**
 * 从环境读数据根：`XIANGQI_HOME` → `{GEBAI_HOME}/相期` → `<cwd>/相期`。
 *
 * 为何要认 `GEBAI_HOME`：宿主进程的 cwd 可能是仓库目录或任意工作目录，
 * 直接往 cwd 写会在那里凭空长出 `相期/`（实测：跑一次测试仓库里多了一个目录）。
 * 歌白的数据本来就在 `GEBAI_HOME` 下，把相期的箱件放那里才符合“一份部署一份数据”。
 */
function homeFromEnv(): string {
  const raw = process.env.XIANGQI_HOME
  if (raw !== undefined && raw.trim() !== "") return raw.trim()
  const home = process.env.GEBAI_HOME
  if (home !== undefined && home.trim() !== "") return join(home.trim(), "相期")
  return join(process.cwd(), "相期")
}

/**
 * 造一个控制器。
 *
 * **懒监听**：第一次 `push` 时才 bind。理由——大多数会话根本不会用到相期，
 * 一进场就占一个端口是白付代价。
 */
export function createController(options: ControllerOptions = {}): XiangqiController {
  const port = options.port ?? portFromEnv()
  const hostname = options.hostname ?? DEFAULT_HOST
  const now = options.now ?? (() => Date.now())
  /** 相期数据根；空串 = 禁用文件通路。 */
  const homeDir = options.homeDir ?? homeFromEnv()
  const outboxPollMs = options.outboxPollMs ?? 500

  /** 当前连着的相期窗口（多个同时连只认最后一个——窗口本就该只有一个）。 */
  let peer: PeerSocket | null = null
  let server: { port?: number; stop(force?: boolean): void } | null = null
  /** 已投递、仍等答复的卡（id → pending）。 */
  const waiting = new Map<string, PendingCard>()
  /** 所有已投递但未被答复的卡（含不等答复的 notice）。用于 dismiss_all 与状态汇报。 */
  const live = new Map<string, CardSpec & { id: string }>()
  let seq = 0
  let closed = false
  /** outbox 轮询句柄。 */
  let pollTimer: ReturnType<typeof setInterval> | null = null
  /** 已处理过的 outbox 文件名（幂等：轮询会反复看到同一个文件）。 */
  const consumedOutbox = new Set<string>()

  function inboxDir(): string {
    return join(homeDir, "inbox")
  }

  function outboxDir(): string {
    return join(homeDir, "outbox")
  }

  /** 文件通路是否可用（`homeDir` 为空串时关闭）。 */
  function filePathEnabled(): boolean {
    return homeDir !== ""
  }

  function url(): string {
    // `port=0` 时端口是 bind 之后才知道的，所以读地址隐含“先起好”。
    // 不自动 listen 的话调用方会拿到 `:0` 这个无意义的地址（测试里就是这样连不上的）。
    if (server === null) listen()
    const actual = server?.port ?? port
    return `ws://${hostname}:${actual}/xiangqi`
  }

  function isConnected(): boolean {
    return peer !== null
  }

  function sendCards(): void {
  if (peer === null) return
  // 顺带剔除已过期的卡（TTL 到期的链接已失效，留着只会引导用户去点一个废链接）
  const nowMs = now()
  for (const [id, card] of live) {
    if (card.expiresAtMs !== undefined && card.expiresAtMs <= nowMs) {
      live.delete(id)
      // 过期卡不唤醒等待者：等待者自己会超时，语义上「卡片过期」≠「用户答了」
    }
  }
  try {
    peer.send(JSON.stringify({ type: "cards", cards: [...live.values()] }))
  } catch {
    // 发送失败：连接已坏，等它 close。卡片仍在 `live` 里，重连后会重推。
  }
}

  function handleMessage(raw: unknown): void {
    if (typeof raw !== "string") return
    let parsed: unknown
    try {
      parsed = JSON.parse(raw)
    } catch {
      return // 脏数据不该炸掉控制器
    }
    if (typeof parsed !== "object" || parsed === null) return
    const message = parsed as Record<string, unknown>
    if (message.type !== "answer") return
    const id = typeof message.id === "string" ? message.id : ""
    if (id === "") return
    const kind = (message.kind as CardKind) ?? "notice"
    const answer = (message.answer as Record<string, unknown>) ?? {}
    deliver({ id, kind, answer, answeredAtMs: now() })
  }

  /**
   * 已到达但**当时没人在等**的答复（先到先存）。
   *
   * 为何需要：用户可能点得很快（一拍完按钮就点），而 `card_ask` 还没开始等。
   * 没有这个缓存的话，那条答复会在 `pollOutbox` 里因为“`live`/`waiting` 都没有它”
   * 而被当成陈旧文件丢掉，然后 `waitForAnswer` 白等满整个超时。
   */
  const arrivedEarly = new Map<string, CardAnswer>()

  /** 收到答复：从 live 摘掉、唤醒等待者。WS 与 outbox 两条路都走这里。 */
  function deliver(record: CardAnswer): void {
    live.delete(record.id)
    const pendingCard = waiting.get(record.id)
    if (pendingCard !== undefined) {
      waiting.delete(record.id)
      pendingCard.resolve(record)
      return
    }
    // 没人在等（可能还没开始等）→ 存起来，等 `waitForAnswer` 来取
    arrivedEarly.set(record.id, record)
  }

  /**
   * 扫一次 `outbox/`，把还没消费的答复捡回来。
   *
   * 为何需要轮询而不是“只在超时前扫一次”：答复可能在 WS 断期间到达，
   * 而那时可能没人在等（`card_ask` 已超时返回）。轮询能把它及时消化掉，
   * 也能让下一张卡的等待不被旧文件干扰。
   *
   * 幂等：同一个文件名只处理一次（`consumedOutbox`），因为文件不会被删
   * （相期不知道我们什时候扫，删了反而丢证据）。
   */
  function pollOutbox(): void {
    if (!filePathEnabled() || closed) return
    const dir = outboxDir()
    if (!existsSync(dir)) return
    let names: string[]
    try {
      names = readdirSync(dir)
    } catch {
      return   // 目录刚被删/无权限：下轮再试
    }
    for (const name of names) {
      if (!name.endsWith(".json")) continue
      if (consumedOutbox.has(name)) continue
      let parsed: unknown
      try {
        parsed = JSON.parse(readFileSync(join(dir, name), "utf-8"))
      } catch {
        // 半写状态（正在写）/ 坏文件：**不标记已消费**，下轮再看。
        // 直接跳过会在“刚好读到中间态”时永久丢掉一条答复。
        continue
      }
      if (typeof parsed !== "object" || parsed === null) {
        consumedOutbox.add(name)
        continue
      }
      const record = parsed as Record<string, unknown>
      if (record.type !== "answer") {
        consumedOutbox.add(name)
        continue
      }
      const id = typeof record.id === "string" ? record.id : ""
      if (id === "") {
        consumedOutbox.add(name)
        continue
      }
      consumedOutbox.add(name)
      // 已处理过的答复不重复投递（WS 与 outbox 两边都到了的情况）。
      //
      // 判据是“已被我们知晓”：仍在 `live`（还没人处理）或 `waiting`（有人在等）
      // 或 `arrivedEarly`（先到了存着）。三者都不在 = 陈旧文件（上个进程留下的，
      // 或我们已经消费过）——忽略比乱投安全。
      if (!live.has(id) && !waiting.has(id) && !arrivedEarly.has(id)) {
        continue
      }
      deliver({
        id,
        kind: (record.kind as CardKind) ?? "notice",
        answer: (record.answer as Record<string, unknown>) ?? {},
        answeredAtMs: now(),
      })
    }
  }

  /** 启 outbox 轮询（懒：第一次需要时启）。 */
  function startPolling(): void {
    if (!filePathEnabled() || outboxPollMs <= 0 || pollTimer !== null || closed) return
    pollTimer = setInterval(pollOutbox, outboxPollMs)
    // 不阻止进程退出：这是个后台活
    if (typeof pollTimer === "object" && pollTimer !== null && "unref" in pollTimer) {
      ;(pollTimer as unknown as { unref: () => void }).unref()
    }
  }

  /**
   * 降级推送：把卡写到 `inbox/`（相期启动时会 `poll_inbox()` 吃掉）。
   *
   * 为何只在**未连接**时写：WS 通了就以 WS 为准（整批替换）。
   * 两路同时送会让同一张卡在窗口上出现两次（文件是追加语义，不知道整批）。
   *
   * 返回 true = 写成功（至少相期下次启动能看到）。
   */
  function writeInbox(card: CardSpec & { id: string }): boolean {
    if (!filePathEnabled()) return false
    try {
      mkdirSync(inboxDir(), { recursive: true })
      // 文件名用 id：同名覆盖而不是堆积（重推同一张卡不该产生两个文件）
      writeFileSync(join(inboxDir(), `${card.id}.json`), JSON.stringify(card, null, 2), "utf-8")
      return true
    } catch {
      return false
    }
  }

  function listen(): void {
    if (closed || server !== null) return
    try {
      server = Bun.serve({
        port,
        hostname,
        fetch(request, srv) {
          // 只接 `/xiangqi` 这一条路径：控制器端口是本机专用的，
          // 别的路径不该被当成 WebSocket 升级。
          if (new URL(request.url).pathname === "/xiangqi" && srv.upgrade(request)) {
            return undefined
          }
          return new Response("not a xiangqi endpoint", { status: 404 })
        },
        websocket: {
          open(ws) {
            // 只认一个窗口：已经有一个连着时，新连接取代旧的。
            // 反过来的话（旧的留着、新的被拒）用户重启相期后就再也推不动卡片了。
            peer = ws as unknown as PeerSocket
            if (live.size > 0) sendCards()
          },
          message(_ws, data) {
            handleMessage(String(data))
          },
          close() {
            peer = null
          },
        },
      }) as unknown as { port?: number; stop(force?: boolean): void }
    } catch (error) {
      // 端口被占（常见：上一个 gebai 没退干净）——**不抛**，否则装载本子Agent
      // 就直接失败了。让 `push` 如实报“送不到”，那比启动崩掉可诊断得多。
      server = null
      void error
    }
  }

  function nextId(): string {
    seq += 1
    return `card-${now().toString(36)}-${seq}`
  }

  /**
   * 投递一张卡，返回 `{ id, delivered }`。
   *
   * ## 为何不抛错
   *
   * 最初写的是“未连接就抛”，但那语义矛盾：卡片**已经进 `live` 了**
   * （连上会重推），却告诉调用方失败。调用方会重试 → 同一件事推两张卡，
   * 用户回来看到两条一模一样的。
   *
   * 所以改成：**不受连接状态影响地收下，用 `delivered` 如实告诉它此刻能不能送达**。
   * 工具层把 `delivered=false` 写进给模型的文本（“用户看不到”），
   * 而不是抛一个让模型以为“没发出去、该重试”的错。
   */
  function push(spec: CardSpec): { id: string; delivered: boolean } {
    if (closed) throw new Error("相期控制器已关闭")
    const id = spec.id ?? nextId()
    const card = { ...spec, id }

    // 校验：`choice` 没选项等于把用户逼进死路（界面上没有可点的东西）
    if (card.kind === "choice" && (card.options === undefined || card.options.length === 0)) {
      throw new Error("choice 卡必须有至少一个 option（否则用户无法作答）")
    }

    live.set(id, card)
    if (server === null) listen()
    startPolling()
    const viaWs = isConnected()
    if (viaWs) {
      sendCards()
    } else {
      // WS 不通 → 写 inbox 做降级。相期启动/下次 `poll_inbox()` 会吃掉它。
      // 写不进去也不算致命：卡片仍在 `live` 里，连上后会重推。
      writeInbox(card)
    }
    // `delivered` 的含义是“**此刻**用户能实时看到”：
    // 写进 inbox 不算——相期没在跑就不会有人去读那个目录。
    return { id, delivered: viaWs }
  }

  async function waitForAnswer(id: string, timeoutMs: number): Promise<CardAnswer | null> {
    const sentAt = now()
    // 先扫一次 outbox：答复可能**已经**到了（用户点得很快、或 WS 断在
    // 那个瞬间而轮询还没跑到）。不先扫的话这里会白白等满整个超时。
    startPolling()
    pollOutbox()

    // 答复已先到（`deliver` 存进 `arrivedEarly`）→ 立即返回，不用进等待
    const early = arrivedEarly.get(id)
    if (early !== undefined) {
      arrivedEarly.delete(id)
      return early
    }

    return await new Promise<CardAnswer | null>((resolve) => {
      let timer: ReturnType<typeof setTimeout> | null = null

      /** 收尾：清定时器并解下一次性的 resolve。 */
      const settle = (answer: CardAnswer | null): void => {
        if (timer !== null) {
          clearTimeout(timer)
          timer = null
        }
        resolve(answer)
      }

      const pendingCard: PendingCard = {
        spec: live.get(id) ?? ({ id, kind: "notice", title: "" } as PendingCard["spec"]),
        sentAtMs: sentAt,
        // 答复到达时走 settle → 顺手把定时器清掉。
        // 不清的话 `bun test` 这类运行器会等未完成的定时器，
        // 一个 60 秒的等待能把整轮测试拖死（实测踩到）。
        resolve: (answer) => settle(answer),
        cancel: () => settle(null),
      }
      waiting.set(id, pendingCard)

      if (timeoutMs > 0) {
        timer = setTimeout(() => {
          timer = null
          if (waiting.get(id) === pendingCard) {
            waiting.delete(id)
            // 超时**不从 `live` 里删**：卡片还留在窗口上等人处理，
            // 答了以后仍会作为答复回来（只是没人等了）。
            resolve(null)
          }
        }, timeoutMs)
      }
    })
  }

  function pending(): Array<{ id: string; title: string; kind: CardKind; sentAtMs: number }> {
    return [...waiting.entries()].map(([id, card]) => ({
      id,
      title: card.spec.title,
      kind: card.spec.kind,
      sentAtMs: card.sentAtMs,
    }))
  }

  function dismissAll(): void {
    live.clear()
    arrivedEarly.clear()
    if (isConnected()) sendCards()
  }

  /**
   * 只撤一张卡（授权完成后让那张「去授权」卡自动消失）。
   *
   * 为何不能拿 `dismissAll()` 代替：那会把**其他还在等用户处理的**卡片一起清掉——
   * 等于替用户做了「不处理」的决定。单卡撤回是「这件事已经有结果了」的表达，
   * 与「这批都过时了」是两种语义。
   *
   * 已撤回但仍有人等待的卡：唤醒等待者（返回 null），避免它白等到超时。
   */
  function dismiss(id: string): boolean {
    const had = live.delete(id)
    arrivedEarly.delete(id)
    const waiter = waiting.get(id)
    if (waiter) {
      waiting.delete(id)
      waiter.cancel()
    }
    if (had && isConnected()) sendCards()
    return had
  }

  function close(): void {
    closed = true
    if (pollTimer !== null) {
      clearInterval(pollTimer)
      pollTimer = null
    }
    for (const [, card] of waiting) card.cancel()
    waiting.clear()
    live.clear()
    arrivedEarly.clear()
    if (peer !== null) {
      try {
        peer.close()
      } catch {
        // 关不掉也无所谓
      }
      peer = null
    }
    if (server !== null) {
      try {
        server.stop(true)
      } catch {
        // 已经停了
      }
      server = null
    }
  }

  return {
    url,
    isConnected,
    dropPeerForTest: () => {
      if (peer === null) return
      try {
        peer.close()
      } catch {
        // 已经断了的也无所谓
      }
      peer = null
    },
    listen,
    push,
    waitForAnswer,
    pending,
    dismissAll,
    dismiss,
    close,
  }
}

/**
 * **进程级单例**。
 *
 * 为何要单例：端口只能被 bind 一次，窗口也只有一个。多个会话装载本子Agent 时
 * 拿到的是同一个控制器——这是“不抢窗口”的全部机制。
 *
 * 测试需要隔离时用 `resetControllerForTest()`。
 */
let singleton: XiangqiController | null = null

export function controller(options: ControllerOptions = {}): XiangqiController {
  if (singleton === null) singleton = createController(options)
  return singleton
}

/** 仅测试用：丢掉单例（下次 `controller()` 会新建）。 */
export function resetControllerForTest(): void {
  singleton?.close()
  singleton = null
}
