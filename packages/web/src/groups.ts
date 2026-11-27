/**
 * 多人会话（群）前端：轮盘入口 → 面板（我的群 / 创建 / 加入）。
 *
 * 交互形态（与待办/任务面板同一套「右侧抽屉」语言，见 css/groups.css）：
 * - **我的群**：我参与的群列表（群名、我的群内身份、成员数、群主标记），点击即进入该群会话；
 *   右列是当前群的详情（邀请码一键复制、成员表、群主操作：改名/进入口令/转让/解散）与
 *   「我在这个群里的名称与角色」（只影响本群，不动账号）。
 * - **创建**：群名 + 我在群里的名称/角色 + 可选进入口令（留空 = 持邀请码即可进）。
 * - **加入**：邀请码 + （该群有口令时）口令 + 我在群里的名称/角色。
 *
 * 为何名称/角色在**每次进群**时填而不是「跟账号走」：群内身份是这段关系里的称呼，
 * 同一个人在这个群是「主持人」、在那个群是「观察者」——账号只决定「你是谁、能不能进」。
 */
import type { GroupInfo, SessionInfo } from "@gebai/sdk"
import { client, el, getCurrentSession, noteGroupJoinCode, setCurrentSession, updateTitle } from "./state"
import { pushEscScope, popKeyScope } from "./keymap"
import { toast } from "./ui"
import { refreshSessions } from "./sessions"

let overlay: HTMLElement | null = null
let body: HTMLElement | null = null
let tab: "list" | "create" | "join" = "list"
/** 当前详情面板展示的群（列表里点选）。 */
let selected: string | null = null
let groups: GroupInfo[] = []
let scopeId = ""
/** 进入群会话后的回调（main.ts 注入：切会话 = 载入消息 + 重绑 UI）。 */
let openSession: ((session: SessionInfo) => Promise<void>) | null = null

/** 注册「进入群会话」动作（main.ts 启动时注入；未注入时只切当前会话标记）。 */
export function setGroupSessionOpener(fn: (session: SessionInfo) => Promise<void>): void {
  openSession = fn
}

function close(): void {
  if (!overlay) return
  overlay.remove()
  overlay = null
  body = null
  popKeyScope(scopeId)
}

function openPanel(next: "list" | "create" | "join" = "list"): void {
  tab = next
  if (!overlay) {
    overlay = el("div", "settings-overlay")
    const panel = el("div", "settings-panel groups-panel")
    const head = el("div", "settings-head")
    head.append(el("span", "settings-title", "多人会话"))
    const closeBtn = el("button", "icon-btn", "✕") as HTMLButtonElement
    closeBtn.setAttribute("aria-label", "关闭多人会话面板")
    closeBtn.addEventListener("click", close)
    head.append(closeBtn)
    const tabs = el("div", "settings-tabs")
    for (const [key, label] of [
      ["list", "我的群"],
      ["create", "创建"],
      ["join", "加入"],
    ] as const) {
      const b = el("button", "", label) as HTMLButtonElement
      b.dataset.tab = key
      b.addEventListener("click", () => {
        tab = key
        void render()
      })
      tabs.append(b)
    }
    body = el("div", "settings-body")
    panel.append(head, tabs, body)
    overlay.append(panel)
    overlay.addEventListener("pointerdown", (ev) => {
      if (ev.target === overlay) close()
    })
    document.body.append(overlay)
    scopeId = pushEscScope("main.groups", "关闭多人会话面板", close)
  }
  void render()
}

/** 轮盘入口（`#groups-btn`）：点开面板。 */
export function bindGroups(): void {
  const btn = document.getElementById("groups-btn")
  if (!btn) return
  btn.addEventListener("click", (e) => {
    e.preventDefault()
    openPanel("list")
  })
}

async function reload(): Promise<void> {
  try {
    groups = await client.listGroups()
    // 邀请码回填身份条（标题栏那一条要随时能“复制邀请码拉人”）
    for (const g of groups) noteGroupJoinCode(g.id, g.joinCode)
  } catch (err) {
    groups = []
    toast(`群列表加载失败：${(err as Error).message}`, "error")
  }
  if (selected && !groups.some((g) => g.id === selected)) selected = null
}

/** 启动时同步邀请码缓存（不进面板也要能在身份条上复制邀请码）。 */
export async function syncGroupJoinCodes(): Promise<void> {
  try {
    for (const g of await client.listGroups()) noteGroupJoinCode(g.id, g.joinCode)
  } catch {
    /* 离线/未登录：身份条降级为「在面板查看」提示 */
  }
}

async function render(): Promise<void> {
  if (!body) return
  for (const b of Array.from(body.parentElement?.querySelectorAll(".settings-tabs button") ?? [])) {
    b.classList.toggle("active", (b as HTMLElement).dataset.tab === tab)
  }
  body.innerHTML = ""
  if (tab === "create") return renderCreate()
  if (tab === "join") return renderJoin()
  await reload()
  renderList()
}

/* ---------- 我的群 ---------- */

function renderList(): void {
  if (!body) return
  const wrap = el("div", "groups-wrap")
  const list = el("div", "groups-list")
  if (!groups.length) {
    list.append(el("div", "groups-empty", "还没有加入任何多人会话。点「创建」开一个群，或用邀请码「加入」。"))
  }
  const cur = getCurrentSession()
  for (const g of groups) {
    const row = el("div", `groups-row${g.id === selected ? " active" : ""}`) as HTMLElement
    row.dataset.gid = g.id
    const title = el("div", "groups-row-title")
    title.append(el("span", "groups-ico", "👥"), el("span", "groups-name", g.name))
    if (cur?.id === g.sessionId) title.append(el("span", "groups-now", "当前"))
    const meta = el("div", "groups-row-meta")
    const who = g.me ? `${g.me.role}·${g.me.name}` : "—"
    meta.textContent = `${who} · ${g.memberCount} 人${g.owner === g.me?.userId ? " · 群主" : ""}${g.hasPasscode ? " · 有口令" : ""}`
    row.append(title, meta)
    row.addEventListener("click", () => {
      selected = g.id
      void render()
    })
    // 双击直接进会话（单击是看详情）
    row.addEventListener("dblclick", () => void enterGroup(g))
    list.append(row)
  }
  wrap.append(list)
  const detail = el("div", "groups-detail")
  const g = groups.find((x) => x.id === selected) ?? null
  if (g) renderDetail(detail, g)
  else detail.append(el("div", "groups-empty", groups.length ? "选一个群查看详情与成员。" : "群详情会显示在这里。"))
  wrap.append(detail)
  body.append(wrap)
}

function renderDetail(host: HTMLElement, g: GroupInfo): void {
  const head = el("div", "groups-detail-head")
  head.append(el("div", "groups-detail-name", g.name))
  const enter = el("button", "groups-btn primary", "进入会话") as HTMLButtonElement
  enter.addEventListener("click", () => void enterGroup(g))
  head.append(enter)
  host.append(head)

  // 邀请码（加入凭据）：一键复制——这是要发给别人的东西，不能只给看
  const codeRow = el("div", "groups-field")
  codeRow.append(el("label", "", "邀请码"))
  const codeBox = el("div", "groups-code")
  codeBox.append(el("code", "", g.joinCode))
  const copy = el("button", "groups-btn", "复制") as HTMLButtonElement
  copy.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(g.joinCode)
      toast("邀请码已复制", "ok")
    } catch {
      toast("复制失败（请手动选中邀请码）")
    }
  })
  codeBox.append(copy)
  codeRow.append(codeBox)
  host.append(codeRow)
  host.append(el("div", "groups-hint", g.hasPasscode ? "该群设了进入口令，加入时需要口令 + 邀请码。" : "该群没有进入口令：持邀请码即可加入。"))

  // 我在这个群里的身份（改的是群内身份，不动账号）
  const meRow = el("div", "groups-field")
  meRow.append(el("label", "", "我在本群的名称 / 角色"))
  const inputs = el("div", "groups-inline")
  const nameIn = el("input", "groups-input") as HTMLInputElement
  nameIn.value = g.me?.name ?? ""
  nameIn.placeholder = "名称（群内显示）"
  nameIn.maxLength = 32
  const roleIn = el("input", "groups-input") as HTMLInputElement
  roleIn.value = g.me?.role ?? ""
  roleIn.placeholder = "角色（如 主持人 / 工程师）"
  roleIn.maxLength = 16
  const save = el("button", "groups-btn", "保存") as HTMLButtonElement
  save.addEventListener("click", async () => {
    try {
      const next = await client.setGroupProfile(g.id, nameIn.value.trim(), roleIn.value.trim())
      toast("群内身份已更新（之后发言按新身份署名）", "ok")
      applyGroup(next)
    } catch (err) {
      toast(`保存失败：${(err as Error).message}`, "error")
    }
  })
  inputs.append(nameIn, roleIn, save)
  meRow.append(inputs)
  host.append(meRow)

  // 成员表
  const members = el("div", "groups-field")
  members.append(el("label", "", `成员（${g.memberCount}）`))
  const table = el("div", "groups-members")
  for (const m of g.members) {
    const row = el("div", "groups-member")
    row.dataset.uid = m.userId
    row.append(el("span", "groups-member-name", m.name), el("span", "groups-member-role", m.role))
    if (m.owner) row.append(el("span", "groups-badge", "群主"))
    if (m.userId === g.me?.userId) row.append(el("span", "groups-badge", "我"))
    table.append(row)
  }
  members.append(table)
  host.append(members)

  const actions = el("div", "groups-actions")
  if (g.owner === g.me?.userId) {
    const rename = el("button", "groups-btn", "改群名") as HTMLButtonElement
    rename.addEventListener("click", async () => {
      const next = window.prompt("新的群名", g.name)
      if (next == null || !next.trim()) return
      try {
        applyGroup(await client.renameGroup(g.id, next.trim()))
        toast("群名已更新", "ok")
      } catch (err) {
        toast(`改名失败：${(err as Error).message}`, "error")
      }
    })
    const passcode = el("button", "groups-btn", g.hasPasscode ? "改/清除口令" : "设置进入口令") as HTMLButtonElement
    passcode.addEventListener("click", async () => {
      const next = window.prompt(g.hasPasscode ? "新的进入口令（留空 = 取消口令）" : "进入口令（留空 = 不设）", "")
      if (next == null) return
      try {
        applyGroup(await client.setGroupPasscode(g.id, next.trim() || null))
        toast(next.trim() ? "进入口令已设置" : "已取消进入口令", "ok")
      } catch (err) {
        toast(`设置失败：${(err as Error).message}`, "error")
      }
    })
    const diss = el("button", "groups-btn danger", "解散群") as HTMLButtonElement
    diss.addEventListener("click", async () => {
      if (!window.confirm(`解散「${g.name}」？群会话与全部消息将被删除，所有成员都会失去这个会话。`)) return
      try {
        await client.dissolveGroup(g.id)
        toast("群已解散", "ok")
        selected = null
        await reload()
        await refreshSessions()
        renderList()
      } catch (err) {
        toast(`解散失败：${(err as Error).message}`, "error")
      }
    })
    actions.append(rename, passcode, diss)
    host.append(actions)
  } else {
    const leave = el("button", "groups-btn danger", "退出这个群") as HTMLButtonElement
    leave.addEventListener("click", async () => {
      if (!window.confirm(`退出「${g.name}」？之后你将看不到这个群的会话。`)) return
      try {
        await client.leaveGroup(g.id)
        toast("已退出群", "ok")
        selected = null
        await reload()
        await refreshSessions()
        renderList()
      } catch (err) {
        toast(`退出失败：${(err as Error).message}`, "error")
      }
    })
    actions.append(leave)
    host.append(actions)
  }
}

/** 群视图更新后就地刷新（保存身份/改名后不必整表重拉）。 */
function applyGroup(next: GroupInfo): void {
  const i = groups.findIndex((x) => x.id === next.id)
  if (i >= 0) groups[i] = next
  else groups.push(next)
  // 当前正开着这个群会话：会话对象是 state 的**活引用**，就地换掉群内身份与群名，
  // 标题栏身份条与标题立即跟着变（不必等下一次 session.list 回填）
  const cur = getCurrentSession()
  if (cur?.group?.id === next.id) {
    cur.group = { id: next.id, name: next.name, memberName: next.me?.name ?? "", role: next.me?.role ?? "" }
    cur.name = next.name
    updateTitle()
  }
  // 会话列表里的群内身份也要跟着变（列表按人渲染「我在这群叫什么」）
  void refreshSessions()
  if (body) {
    body.innerHTML = ""
    renderList()
  }
}

async function enterGroup(g: GroupInfo): Promise<void> {
  try {
    // 会话信息以服务端为准（列表里的 SessionInfo 可能已陈旧）；群会话的访问权由服务端成员表裁定
    const sessions = await client.listSessions()
    const s = sessions.find((x) => x.id === g.sessionId) ?? null
    if (!s) {
      toast("会话不存在（群可能已被解散）", "error")
      return
    }
    close()
    if (openSession) await openSession(s)
    else setCurrentSession(s)
  } catch (err) {
    toast(`进入失败：${(err as Error).message}`, "error")
  }
}

/* ---------- 创建 ---------- */

function renderCreate(): void {
  if (!body) return
  const form = el("div", "groups-form")
  const name = field(form, "群名", "例如：产品评审", 80)
  const memberName = field(form, "我在群里的名称", "例如：张三", 32)
  const memberRole = field(form, "我的角色（可选）", "例如：主持人", 16)
  const passcode = field(form, "进入口令（可选）", "留空 = 持邀请码即可加入", 64, "password")
  form.append(el("div", "groups-hint", "群内名称与角色只属于这个群，不与账号绑定：同一个账号在不同群里可以是不同身份。"))
  const submit = el("button", "groups-btn primary", "创建并进入") as HTMLButtonElement
  submit.addEventListener("click", async () => {
    if (!name.value.trim()) return toast("请填写群名", "error")
    if (!memberName.value.trim()) return toast("请填写你在群里的名称", "error")
    submit.disabled = true
    try {
      const r = await client.createGroup({
        name: name.value.trim(),
        passcode: passcode.value.trim() || undefined,
        memberName: memberName.value.trim(),
        memberRole: memberRole.value.trim() || undefined,
      })
      await afterJoin(r.group, r.session, "群已创建")
    } catch (err) {
      toast(`创建失败：${(err as Error).message}`, "error")
    } finally {
      submit.disabled = false
    }
  })
  form.append(submit)
  body.append(form)
  name.focus()
}

/* ---------- 加入 ---------- */

function renderJoin(): void {
  if (!body) return
  const form = el("div", "groups-form")
  const code = field(form, "邀请码", "8 位邀请码（不区分大小写）", 16)
  const passcode = field(form, "进入口令（该群设了口令时必填）", "没有口令的群可留空", 64, "password")
  const memberName = field(form, "我在群里的名称", "例如：李四", 32)
  const memberRole = field(form, "我的角色（可选）", "例如：工程师", 16)
  form.append(el("div", "groups-hint", "已经是成员时再「加入」一次 = 更新你在该群的名称与角色。"))
  const submit = el("button", "groups-btn primary", "加入并进入") as HTMLButtonElement
  submit.addEventListener("click", async () => {
    if (!code.value.trim()) return toast("请填写邀请码", "error")
    if (!memberName.value.trim()) return toast("请填写你在群里的名称", "error")
    submit.disabled = true
    try {
      const r = await client.joinGroup({
        joinCode: code.value.trim(),
        passcode: passcode.value.trim() || undefined,
        memberName: memberName.value.trim(),
        memberRole: memberRole.value.trim() || undefined,
      })
      await afterJoin(r.group, r.session, "已加入群")
    } catch (err) {
      toast(`加入失败：${(err as Error).message}`, "error")
    } finally {
      submit.disabled = false
    }
  })
  form.append(submit)
  body.append(form)
  code.focus()
}

/** 建群/入群之后：关面板 → 刷新会话列表 → 直接进入群会话。 */
async function afterJoin(g: GroupInfo, session: SessionInfo | null, okText: string): Promise<void> {
  toast(`${okText}「${g.name}」`, "ok")
  // 邀请码先入缓存：身份条与会话列表都会立即读它（不等下一次列表刷新）
  noteGroupJoinCode(g.id, g.joinCode)
  close()
  await refreshSessions()
  const s = session ?? (await client.listSessions()).find((x) => x.id === g.sessionId) ?? null
  if (!s) return
  if (!s.group) {
    // 应答里的会话项没带群归属（旧版服务端）：按自己的身份补上，界面不会停在“普通会话”形态
    s.group = { id: g.id, name: g.name, memberName: g.me?.name ?? "", role: g.me?.role ?? "" }
  }
  if (openSession) await openSession(s)
  else setCurrentSession(s)
}

/** 表单一行：label + input（返回 input 供读取）。 */
function field(host: HTMLElement, label: string, placeholder: string, maxLength: number, type = "text"): HTMLInputElement {
  const row = el("div", "groups-field")
  row.append(el("label", "", label))
  const input = el("input", "groups-input") as HTMLInputElement
  input.type = type
  input.placeholder = placeholder
  input.maxLength = maxLength
  input.autocomplete = "off"
  row.append(input)
  host.append(row)
  return input
}
