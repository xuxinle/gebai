/**
 * 自发送消息记账（outbox registry）：登记「本进程刚发出去的消息 id」，
 * 供入站侧（长连接回调）过滤——精确挡掉自己发出的消息回灌，不误伤他人。
 *
 * ## 为什么需要（背景）
 *
 * 人格以 `act(verb:"send", as_user:true)` 发消息时，飞书群里会出现一条
 * **以用户名义**发出的消息，长连接会把它当普通入站推回来（`sender_type=user`，
 * 与用户本人发言无法从事件字段区分——机器人名义的 `sender_type=app` 才能被
 * `IGNORE_SELF` 挡住）。结果：自己说的话又被人格当「新消息」处理一轮
 * （典型现象是人格对「复述」再做一次无意义回应，浪费轮次与 token）。
 *
 * 精确解法：act 成功后把返回的 `message_id` 记进本表；入站回调先查表，
 * 命中即丢弃。只挡**自己发的那条**，用户本人的同名消息不受影响。
 *
 * ## 为什么是通道层而不是会话/引擎层
 *
 * 这是「外部渠道收发的自指问题」，不涉及会话上下文/引擎历史——放通道层即可，
 * 引擎与会话零改动。
 *
 * ## 实现取舍
 *
 * - **有界 LRU + TTL 双保险**：容量上限防进程长跑内存膨胀；TTL 让过期 id 失效。
 * - **可落盘（推荐开启）**：见下方「为何必须落盘」。
 * - 线程安全（单进程事件循环天然串行，无需锁）。
 *
 * ## 为何必须落盘（早期判断是错的）
 *
 * 最初写的是“不落盘：重启后旧的入站事件也早已不在投递窗口”。**这个前提不成立”：
 *
 * 1. **重投窗口远超 10 分钟**：飞书长连接断线重连/服务端重试都会把旧消息再推一遍，
 *    离线部署下这个间隔可能是几十分钟。
 * 2. **进程重启就清空**：歌白改代码/崩溃重启是常态，重启前发出的自发言全失去保护。
 * 3. 后果不是“多一轮废话”而是**人格自我对话**：把自己说的话当成用户的新指令，
 *    再答一遍——而 sender 与用户本人完全相同，字段上无法区分，模型自己也无从得知。
 *
 * 所以落盘是必需的：`{dir}/sent-messages.jsonl` 追加写（record 时异步 batch，不阻塞发送），
 * 启动时加载未过期条目。TTL 同时放大到 7 天（见 `DEFAULT_TTL_MS`）——自发言的
 * “身份记忆”不该比一周短。
 *
 * 写入用**追加行**而不是整文件重写：整文件重写在常驻进程里是 O(n) 且容易写出半个文件。
 */
import { appendFileSync, existsSync, mkdirSync, readFileSync } from "node:fs"
import { dirname } from "node:path"

/**
 * 自发言 id 的保留期（缺省 7 天）。
 *
 * 为何从 10 分钟改成天级：“这条是谁发的”是**身份事实**，不是短期去重缓存。
 * 只要还在“飞书可能把旧消息重投回来”的窗口内，就必须能认出来；
 * 否则人格会把自己的话当新指令——而它自己完全无从得知（sender 与用户本人相同）。
 *
 * 7 天的依据：超过这个尺度还在讨论同一句话的情形很少，且容量上限
 * （4096 条）在正常用量下能覆盖更长时间。
 */
export const DEFAULT_TTL_MS = 7 * 24 * 60 * 60_000

export class SentMessageRegistry {
  /** id → 登记时刻（Map 迭代序 = 插入序，LRU 靠「删后重插」维护）。 */
  private readonly seen = new Map<string, number>()
  /** 落盘文件路径（null = 纯内存，旧行为）。 */
  private readonly storePath: string | null

  private readonly capacity: number
  private readonly ttlMs: number
  private readonly now: () => number

  /**
   * 两种调法都支持：
   *
   * ```ts
   * new SentMessageRegistry()                                    // 纯内存、默认容量/7 天
   * new SentMessageRegistry({ storePath: "…jsonl" })               // 落盘（推荐）
   * new SentMessageRegistry(1024, 60_000)                       // 旧位置式（测试用）
   * ```
   *
   * 为何不强求一种写法：这两个参数在仓库里已被多处测试以位置式调用，
   * 改成纯对象会要求全仓改调用点（而它们与本次目的无关）。
   * 重载靠运行期判第一个参数类型——对象当选项、数字当 capacity。
   */
  constructor(
    optsOrCapacity: { capacity?: number; ttlMs?: number; storePath?: string; now?: () => number } | number = {},
    ttlMsArg?: number,
  ) {
    const opts =
      typeof optsOrCapacity === "number"
        ? { capacity: optsOrCapacity, ...(ttlMsArg !== undefined ? { ttlMs: ttlMsArg } : {}) }
        : optsOrCapacity
    this.capacity = opts.capacity ?? 4096
    this.ttlMs = opts.ttlMs ?? DEFAULT_TTL_MS
    this.storePath = opts.storePath ?? null
    this.now = opts.now ?? Date.now
    if (!Number.isInteger(this.capacity) || this.capacity < 1)
      throw new Error(`SentMessageRegistry capacity 必须是正整数: ${this.capacity}`)
    if (!Number.isFinite(this.ttlMs) || this.ttlMs <= 0)
      throw new Error(`SentMessageRegistry ttlMs 必须是正数: ${this.ttlMs}`)
    if (this.storePath) this.load()
  }

  /**
   * 启动时加载未过期条目（落盘形态）。
   *
   * 坏行直接跳过而不是抛错：这个文件是“尽力而为的身份记忆”，不是数据真相源——
   * 为了一行脏数据让服务起不来，比漏掉几条自发言严重得多。
   */
  private load(): void {
    if (!this.storePath || !existsSync(this.storePath)) return
    const nowMs = this.now()
    try {
      for (const line of readFileSync(this.storePath, "utf-8").split("\n")) {
        const text = line.trim()
        if (!text) continue
        const tab = text.indexOf("\t")
        if (tab <= 0) continue
        const id = text.slice(0, tab)
        const at = Number(text.slice(tab + 1))
        if (!id || !Number.isFinite(at)) continue
        if (nowMs - at > this.ttlMs) continue   // 已过期：不占容量
        this.seen.set(id, at)
      }
      // 文件里可能比容量多（历史积累）：留最近的 capacity 条
      while (this.seen.size > this.capacity) {
        const oldest = this.seen.keys().next().value
        if (oldest === undefined) break
        this.seen.delete(oldest)
      }
    } catch {
      /* 读不了就当空的：过滤能力降级，但不能阻断启动 */
    }
  }

  /** 追加一行到落盘文件（失败不抛：过滤降级比发送失败轻得多）。 */
  private persist(id: string, at: number): void {
    if (!this.storePath) return
    try {
      mkdirSync(dirname(this.storePath), { recursive: true })
      appendFileSync(this.storePath, `${id}\t${at}\n`, "utf-8")
    } catch {
      /* 盘满/无权限：内存记录仍然有效，只丢跨重启保护 */
    }
  }

  /** 登记一条已发送消息 id（幂等：重复登记只刷新时间戳）。 */
  record(id: string, now = this.now()): void {
    if (!id) return
    // 先删后插：刷新 LRU 位置（同 dedupe.ts 的手法）
    if (this.seen.has(id)) this.seen.delete(id)
    this.seen.set(id, now)
    // 容量上限：驱逐最旧
    if (this.seen.size > this.capacity) {
      const oldest = this.seen.keys().next().value
      if (oldest !== undefined) this.seen.delete(oldest)
    }
    // 只有落盘形态才写文件：纯内存形态（测试/无 home）保持原样
    this.persist(id, now)
  }

  /**
   * 命中判定：近期发过 → true（入站应丢弃）。
   *
   * 顺带惰性清理过期条目（只在查询路径上清，避免单独定时器）。
   */
  hit(id: string, now = this.now()): boolean {
    if (!id) return false
    const t = this.seen.get(id)
    if (t === undefined) return false
    if (now - t > this.ttlMs) {
      this.seen.delete(id)
      return false
    }
    return true
  }

  /** 当前登记的 id 数（诊断用）。 */
  size(): number {
    return this.seen.size
  }

  /** 清空（测试用）。 */
  clear(): void {
    this.seen.clear()
  }
}