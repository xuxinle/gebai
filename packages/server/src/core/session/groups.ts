/**
 * 多人会话（群）：**不属于任何用户**的共享会话——单独一棵目录体系
 * `{GEBAI_HOME}/groups/{groupId}/`（`group.json` 群档案 + `chat.json` 会话正文 + `tmp/` 附件与沙箱根）。
 *
 * 为什么不放在用户目录下：群是多人的共同财产，放进某个用户的 `users/{u}/sessions/` 就等于把
 * 它的生命周期、隔离边界与删除权都绑在那一个人身上（成员一离开或该用户被清理，群随之消失）。
 * 群目录与用户目录平级，成员只是「有权进这间房间的人」。
 *
 * **名称与角色是群内身份，不与账号绑定**：同一个账号在不同群里可以是「张三·主持人」与
 * 「Alex·观察者」两副面孔（`GroupMember.name/role`），账号本身（`userId/username`）只用于判定
 * 成员资格与事件投递。这是「进入会话设置名称和角色」的直接落点。
 *
 * 访问模型：
 * - **邀请码**（`joinCode`，8 位无歧义大写字母/数字）是所有成员进入的凭据，创建时生成；
 * - **进入口令**（`passcode`，可选）是群主加的一道门：scrypt 加盐哈希落盘（与账号口令同口径），
 *   明文只在创建/设置那一次经过内存；
 * - 成员资格是**唯一**的读写判据（引擎、存储、事件投递、文件接口共用）。
 *
 * 会话正文仍由 `SessionStore` 管：本类只负责「群是谁的、谁能进、进来叫什么」，
 * 会话目录定位经 `paths.registerGroupSessionDir` 的进程内映射交给 `sessionPath` 统一裁决。
 */
import { randomBytes, scrypt as scryptCb, timingSafeEqual } from "node:crypto"
import { promisify } from "node:util"
import { mkdir, readFile, readdir, rename, rm, writeFile } from "node:fs/promises"
import { join } from "node:path"
import {
  groupDir,
  isValidGroupId,
  registerGroupSessionDir,
  unregisterGroupSessionDir,
} from "../base/paths"
import { log } from "@gebai/sdk/node"

const scrypt = promisify(scryptCb) as (password: string | Buffer, salt: string | Buffer, keylen: number) => Promise<Buffer>

/** 群档案文件名（群目录内，与 chat.json 并列）。 */
export const GROUP_FILE = "group.json"
/** 群名长度上限（与未命名新会话的命名口径一致的 UI 体验）。 */
export const GROUP_NAME_MAX = 80
/** 群成员显示名/角色长度上限。 */
export const MEMBER_NAME_MAX = 32
export const MEMBER_ROLE_MAX = 16
/** 单群成员上限（防一个群把全站用户都拉进来后事件扇出失控）。 */
export const MAX_GROUP_MEMBERS = 200
/** 邀请码字母表：去掉易混的 0/O/1/I，手抄口头相传不会错。 */
const JOIN_CODE_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"
const JOIN_CODE_LEN = 8

/** 群成员（群内身份）。 */
export interface GroupMember {
  /** 账号 id（成员资格与事件投递判据）。 */
  userId: string
  /** 账号名（展示「这个身份背后是谁」的辅助信息，不是群内身份）。 */
  username: string
  /** 群内显示名称（进入群时自设，可与账号名无关）。 */
  name: string
  /** 群内角色（进入群时自设，如「主持人」「工程师」「观察者」）。 */
  role: string
  joinedAt: number
}

/** 群档案（`groups/{id}/group.json`）。 */
export interface GroupRecord {
  id: string
  /** 群名（会话标题同源）。 */
  name: string
  /** 群主账号 id（解散权）。 */
  owner: string
  /** 群会话 id（chat.json 所在会话；一个群一个会话）。 */
  sessionId: string
  /** 邀请码（加入凭据）。 */
  joinCode: string
  /** 进入口令（可选）：scrypt 加盐哈希，明文不落盘。 */
  passcode?: { salt: string; hash: string }
  members: GroupMember[]
  createdAt: number
  updatedAt: number
}

/** 群会话的对外摘要（并入会话列表用；`name/role` 是**当前请求者**在群内的身份）。 */
export interface GroupSessionSummary {
  sessionId: string
  groupId: string
  name: string
  memberName: string
  role: string
  memberCount: number
  owner: boolean
}

/** 群的对外视图（不含口令哈希；含邀请码——这是要发出去的凭据）。 */
export interface GroupView {
  id: string
  name: string
  sessionId: string
  owner: string
  joinCode: string
  /** 是否设了进入口令（口令本身永不回传）。 */
  hasPasscode: boolean
  memberCount: number
  /** 当前用户在群内的身份（非成员时为空）。 */
  me?: { name: string; role: string; userId: string; username: string }
  members: Array<{ userId: string; username: string; name: string; role: string; joinedAt: number; owner: boolean }>
  createdAt: number
  updatedAt: number
}

function newError(text: string): Error {
  return new Error(text)
}

/** 进群身份清洗：空则拒绝（群内必须有个称呼，否则消息前缀无从显示谁在说话）。 */
export function normalizeMember(name: unknown, role: unknown): { name: string; role: string } {
  const n = String(name ?? "").replace(/\s+/g, " ").trim()
  if (!n) throw newError("请填写你在群内的名称")
  const r = String(role ?? "").replace(/\s+/g, " ").trim()
  return { name: n.slice(0, MEMBER_NAME_MAX), role: (r || "成员").slice(0, MEMBER_ROLE_MAX) }
}

/** 群名清洗（空则不合法）。 */
export function normalizeGroupName(name: unknown): string {
  const n = String(name ?? "").replace(/\s+/g, " ").trim()
  if (!n) throw newError("群名不能为空")
  return n.slice(0, GROUP_NAME_MAX)
}

/** 邀请码规范化（去空白、转大写）：手抄带空格/小写也能进。 */
export function normalizeJoinCode(code: unknown): string {
  return String(code ?? "").replace(/[\s-]/g, "").toUpperCase()
}

function generateJoinCode(): string {
  const bytes = randomBytes(JOIN_CODE_LEN)
  let out = ""
  for (let i = 0; i < JOIN_CODE_LEN; i++) out += JOIN_CODE_ALPHABET[bytes[i] % JOIN_CODE_ALPHABET.length]
  return out
}

async function hashPasscode(passcode: string, salt: string): Promise<string> {
  return (await scrypt(passcode, salt, 64)).toString("hex")
}

async function verifyPasscode(passcode: string, stored: { salt: string; hash: string }): Promise<boolean> {
  const actual = Buffer.from(await hashPasscode(passcode, stored.salt), "hex")
  const expected = Buffer.from(stored.hash, "hex")
  if (actual.length !== expected.length) return false
  return timingSafeEqual(actual, expected)
}

/** 会话正文存储的最小面（GroupStore 只用到建会话/改名/登记/注销四件事）。 */
export interface GroupSessionPort {
  createGroupSession(groupId: string, owner: string, name: string): Promise<{ id: string }>
  rename(sessionId: string, name: string, userId?: string): Promise<void>
  /** 登记群归属（群加载/创建/改名时调用）：会话存储与文件系统映射都靠它认识群会话。 */
  register(sessionId: string, group: { groupId: string; name: string }): void
  /** 注销群归属（解散）：之后该会话不再被当作群会话定位。 */
  unregister(sessionId: string): void
  evict(sessionId: string): void
}

export class GroupStore {
  private groups = new Map<string, GroupRecord>()
  /** 会话 id → 群 id（事件投递与成员判定走这里，O(1)）。 */
  private bySession = new Map<string, string>()
  private port: GroupSessionPort | null = null

  constructor(private home: string) {}

  /** 接上会话存储（组会话正文的建立/改名）；已加载的群全部补登记（init 可能先于 attach 跑）。 */
  attach(port: GroupSessionPort): void {
    this.port = port
    for (const rec of this.groups.values()) this.remember(rec)
  }

  /** 启动期扫描（幂等）：加载全部群档案并注册会话目录映射——重启后 group 会话仍能定位到自己的目录。 */
  async init(): Promise<number> {
    let entries: import("node:fs").Dirent[]
    try {
      entries = await readdir(join(this.home, "groups"), { withFileTypes: true })
    } catch {
      return 0 // 尚未有群
    }
    let loaded = 0
    for (const e of entries) {
      if (!e.isDirectory()) continue
      try {
        const rec = JSON.parse(await readFile(join(groupDir(this.home, e.name), GROUP_FILE), "utf8")) as GroupRecord
        if (!rec?.id || rec.id !== e.name || !rec.sessionId) continue
        this.remember(rec)
        loaded++
      } catch {
        /* 损坏档案跳过（不影响其他群） */
      }
    }
    if (loaded) log.info(`[gebai] 多人会话：已加载 ${loaded} 个群`)
    return loaded
  }

  private remember(rec: GroupRecord): void {
    this.groups.set(rec.id, rec)
    this.bySession.set(rec.sessionId, rec.id)
    registerGroupSessionDir(this.home, rec.id, rec.sessionId)
    // 会话存储也须同时认识这个群会话（会话列表只列「记得的群」：仅维护文件系统映射不够，
    // 加入的群在新进程里从未被 load 过，不登记就会在列表里静默消失）
    this.port?.register(rec.sessionId, { groupId: rec.id, name: rec.name })
  }

  private async persist(rec: GroupRecord): Promise<void> {
    rec.updatedAt = Date.now()
    const dir = groupDir(this.home, rec.id)
    await mkdir(dir, { recursive: true })
    const target = join(dir, GROUP_FILE)
    const body = JSON.stringify(rec, null, 2)
    // 原子写（tmp + rename）：进程崩溃留在中途的截断 JSON 会让整个群「消失」
    try {
      await writeFile(`${target}.tmp`, body)
      await rename(`${target}.tmp`, target)
    } catch {
      await writeFile(target, body)
    }
    this.remember(rec)
  }

  /** 群档案（内部用；未加载返回 null）。 */
  get(groupId: string): GroupRecord | null {
    if (!isValidGroupId(groupId)) return null
    return this.groups.get(groupId) ?? null
  }

  /** 会话所属的群（非群会话返回 null）。 */
  groupOfSession(sessionId: string): GroupRecord | null {
    const gid = this.bySession.get(sessionId)
    return gid ? (this.groups.get(gid) ?? null) : null
  }

  /** 成员资格判定（存储/引擎/事件投递共用）：非成员返回 null。 */
  memberOf(sessionId: string, userId: string): GroupMember | null {
    const g = this.groupOfSession(sessionId)
    if (!g) return null
    return g.members.find((m) => m.userId === userId) ?? null
  }

  /** 会话事件受众（群会话 → 全部成员的账号 id；非群会话 → null 表示「走会话归属单播」）。 */
  audienceOf(sessionId: string): string[] | null {
    const g = this.groupOfSession(sessionId)
    if (!g) return null
    return g.members.map((m) => m.userId)
  }

  /** 说话人身份（消息前缀用）：非群会话/非成员返回 null。 */
  speakerOf(sessionId: string, userId: string): { name: string; role: string } | null {
    const m = this.memberOf(sessionId, userId)
    return m ? { name: m.name, role: m.role } : null
  }

  /** 该用户参与的全部群会话（会话列表并入用）。 */
  listFor(userId: string): GroupSessionSummary[] {
    const out: GroupSessionSummary[] = []
    for (const g of this.groups.values()) {
      const m = g.members.find((x) => x.userId === userId)
      if (!m) continue
      out.push({ sessionId: g.sessionId, groupId: g.id, name: g.name, memberName: m.name, role: m.role, memberCount: g.members.length, owner: g.owner === userId })
    }
    return out
  }

  /** 创建群（创建者即群主并自动进入，身份 = 创建时填的名称/角色）。 */
  async create(userId: string, username: string, opts: { name?: unknown; passcode?: unknown; memberName?: unknown; memberRole?: unknown }): Promise<GroupRecord> {
    if (!this.port) throw newError("会话存储未接线")
    const name = normalizeGroupName(opts.name)
    const me = normalizeMember(opts.memberName, opts.memberRole)
    const id = randomBytes(16).toString("hex")
    const session = await this.port.createGroupSession(id, userId, name)
    const record: GroupRecord = {
      id,
      name,
      owner: userId,
      sessionId: session.id,
      joinCode: await this.uniqueJoinCode(),
      members: [{ userId, username, name: me.name, role: me.role, joinedAt: Date.now() }],
      createdAt: Date.now(),
      updatedAt: Date.now(),
    }
    // 会话 id 由 port 分配：**映射先于落盘**（chat.json 的写入依赖 sessionPath 命中群目录，
    // 否则正文会被写进用户目录分片）
    registerGroupSessionDir(this.home, id, session.id)
    const passcode = String(opts.passcode ?? "").trim()
    if (passcode) {
      const salt = randomBytes(16).toString("hex")
      record.passcode = { salt, hash: await hashPasscode(passcode, salt) }
    }
    await this.persist(record)
    return record
  }

  private async uniqueJoinCode(): Promise<string> {
    const used = new Set([...this.groups.values()].map((g) => g.joinCode))
    for (let i = 0; i < 64; i++) {
      const code = generateJoinCode()
      if (!used.has(code)) return code
    }
    throw newError("邀请码生成失败（请重试）")
  }

  /** 按邀请码找群（加入路径；大小写/空格容忍）。 */
  findByJoinCode(code: unknown): GroupRecord | null {
    const want = normalizeJoinCode(code)
    if (want.length !== JOIN_CODE_LEN) return null
    for (const g of this.groups.values()) if (g.joinCode === want) return g
    return null
  }

  /**
   * 加入群（已是成员则按新身份改写——「再进一次」就是改群内名称/角色，不必先退出）。
   * 口令校验在有口令时强制；错误口令与不存在邀请码的应答由调用方统一措辞（不泄露「码对不对」）。
   */
  async join(userId: string, username: string, opts: { joinCode?: unknown; passcode?: unknown; memberName?: unknown; memberRole?: unknown }): Promise<GroupRecord> {
    const g = this.findByJoinCode(opts.joinCode)
    if (!g) throw newError("邀请码无效或该群已解散")
    if (g.passcode) {
      const passcode = String(opts.passcode ?? "")
      if (!passcode) throw newError("该群设有进入口令，请填写口令")
      if (!(await verifyPasscode(passcode, g.passcode))) throw newError("进入口令不正确")
    }
    const me = normalizeMember(opts.memberName, opts.memberRole)
    const existing = g.members.find((m) => m.userId === userId)
    if (existing) {
      existing.name = me.name
      existing.role = me.role
      existing.username = username
    } else {
      if (g.members.length >= MAX_GROUP_MEMBERS) throw newError(`该群成员已满（上限 ${MAX_GROUP_MEMBERS} 人）`)
      g.members.push({ userId, username, name: me.name, role: me.role, joinedAt: Date.now() })
    }
    await this.persist(g)
    return g
  }

  /** 改群内身份（名称/角色）：只影响本群，不动账号。 */
  async setProfile(userId: string, groupId: string, name: unknown, role: unknown): Promise<GroupRecord> {
    const g = this.get(groupId)
    if (!g) throw newError("群不存在")
    const m = g.members.find((x) => x.userId === userId)
    if (!m) throw newError("你不在这个群里")
    const me = normalizeMember(name, role)
    m.name = me.name
    m.role = me.role
    await this.persist(g)
    return g
  }

  /** 改群名（群主）：会话标题同步（列表与标题栏以 chat.json 的 name 为准）。 */
  async rename(userId: string, groupId: string, name: unknown): Promise<GroupRecord> {
    const g = this.get(groupId)
    if (!g) throw newError("群不存在")
    if (g.owner !== userId) throw newError("只有群主可以改群名")
    g.name = normalizeGroupName(name)
    await this.persist(g)
    await this.port?.rename(g.sessionId, g.name, userId)
    return g
  }

  /** 设置/清除进入口令（群主）：null/空串 = 取消口令。 */
  async setPasscode(userId: string, groupId: string, passcode: unknown): Promise<GroupRecord> {
    const g = this.get(groupId)
    if (!g) throw newError("群不存在")
    if (g.owner !== userId) throw newError("只有群主可以设置进入口令")
    const next = String(passcode ?? "").trim()
    if (!next) {
      delete g.passcode
    } else {
      const salt = randomBytes(16).toString("hex")
      g.passcode = { salt, hash: await hashPasscode(next, salt) }
    }
    await this.persist(g)
    return g
  }

  /** 成员离群；群主离群即解散（等同 dissolve，由调用方按返回值执行目录清理）。 */
  async leave(userId: string, groupId: string): Promise<{ dissolved: boolean; sessionId: string }> {
    const g = this.get(groupId)
    if (!g) throw newError("群不存在")
    const idx = g.members.findIndex((m) => m.userId === userId)
    if (idx < 0) throw newError("你不在这个群里")
    if (g.owner === userId) {
      // 群主退群没有「把群留给谁」的隐含规则（自动转移会把权力随手塞给一个可能只想围观的人）：
      // 要么解散，要么把群主交给某个成员（transferOwner）
      throw newError("群主不能直接退出：请先转让群主，或解散该群")
    }
    g.members.splice(idx, 1)
    await this.persist(g)
    return { dissolved: false, sessionId: g.sessionId }
  }

  /** 群主转让（改群主后可自行离群）。 */
  async transferOwner(userId: string, groupId: string, targetUserId: string): Promise<GroupRecord> {
    const g = this.get(groupId)
    if (!g) throw newError("群不存在")
    if (g.owner !== userId) throw newError("只有群主可以转让群主")
    if (!g.members.some((m) => m.userId === targetUserId)) throw newError("目标用户不在该群")
    g.owner = targetUserId
    await this.persist(g)
    return g
  }

  /** 解散群（群主）：返回会话 id 供调用方清理引擎/队列状态；目录删除由调用方确认后执行。 */
  async dissolve(userId: string, groupId: string): Promise<{ sessionId: string }> {
    const g = this.get(groupId)
    if (!g) throw newError("群不存在")
    if (g.owner !== userId) throw newError("只有群主可以解散该群")
    this.groups.delete(g.id)
    this.bySession.delete(g.sessionId)
    unregisterGroupSessionDir(this.home, g.sessionId)
    this.port?.unregister(g.sessionId)
    await rm(groupDir(this.home, g.id), { recursive: true, force: true })
    return { sessionId: g.sessionId }
  }

  /** 群的对外视图（不含口令哈希；邀请码对外可见——它就是要发出去的凭据）。 */
  view(groupId: string, userId?: string): GroupView | null {
    const g = this.get(groupId)
    if (!g) return null
    const me = userId ? g.members.find((m) => m.userId === userId) : undefined
    return {
      id: g.id,
      name: g.name,
      sessionId: g.sessionId,
      owner: g.owner,
      joinCode: g.joinCode,
      hasPasscode: !!g.passcode,
      memberCount: g.members.length,
      ...(me ? { me: { name: me.name, role: me.role, userId: me.userId, username: me.username } } : {}),
      members: g.members
        .slice()
        .sort((a, b) => a.joinedAt - b.joinedAt)
        .map((m) => ({ userId: m.userId, username: m.username, name: m.name, role: m.role, joinedAt: m.joinedAt, owner: g.owner === m.userId })),
      createdAt: g.createdAt,
      updatedAt: g.updatedAt,
    }
  }

  /** 该用户参与的全部群视图（会话列表与「我的群」面板同源）。 */
  listViews(userId: string): GroupView[] {
    const out: GroupView[] = []
    for (const g of this.groups.values()) {
      if (!g.members.some((m) => m.userId === userId)) continue
      const v = this.view(g.id, userId)
      if (v) out.push(v)
    }
    return out.sort((a, b) => b.updatedAt - a.updatedAt)
  }

  /** 系统提示词用的群注记（对话历史里谁在说话、回复给谁看）。 */
  noteFor(sessionId: string, userId?: string): string | undefined {
    const g = this.groupOfSession(sessionId)
    if (!g) return undefined
    const me = userId ? g.members.find((m) => m.userId === userId) : undefined
    const roster = g.members.map((m) => `${m.role}·${m.name}`).join("、")
    return [
      `当前会话是**多人会话（群）**「${g.name}」：一个群成员共同使用的上下文，你的回复对全群可见，不是私聊。`,
      `成员（角色·名称）：${roster}。`,
      `每条用户消息以 \`【角色·名称】\` 前缀标明说话人——按前缀识别是谁在说，回复中需要指代时用该名称，不要用账号名。`,
      me ? `本次任务的发起者是「${me.role}·${me.name}」。` : "",
      `多人协作的注意事项：①不同成员可能提出相互冲突的要求——指出冲突并说明你按谁的要求执行（发起者优先，或询问谁是决策者），不要静默择一；②群里可能有各自熟悉的领域与角色分工，按角色匹配职责（如「主持人」管流程、「工程师」管实现）；③需要用户决定的事（ask 审批/选项/填值）全群都能作答，请在问题里写明要谁定；④不要复述、推断某个成员未在群里说过的私人信息；⑤同一件事不要重复向全群索要，先看前缀里谁已经说过。`,
    ]
      .filter(Boolean)
      .join("\n")
  }
}
