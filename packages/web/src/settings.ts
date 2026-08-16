import type { FeedbackInfo, UserInfo } from "@gebai/sdk"
import { client, el, setConn, settingsBody, settingsBtn, settingsOverlay, settingsTabs } from "./state"
import { blockText } from "./markdown"
import { getLowPowerSetting, isLowPower, probeWebGL, setLowPowerSetting } from "./low-power"
import { loadLocalEnv, saveLocalEnv, filterEnvToCatalog, type EnvCatalogGroup } from "./env-local"
import { confirmDialog, customSelect, toast } from "./ui"

/* ---------- 设置面板（环境变量 / 外观 / 用户 / 反馈；工具启停、子Agent 装载与 Webhook 管理经 API 使用） ---------- */

export function bindSettings() {
  settingsBtn.onclick = () => openSettings()
  const closeBtn = document.getElementById("settings-close")!
  closeBtn.onclick = () => {
    settingsOverlay.hidden = true
  }
  settingsOverlay.addEventListener("click", (e) => {
    if (e.target === settingsOverlay) settingsOverlay.hidden = true
  })
  settingsTabs.addEventListener("click", (e) => {
    const btn = (e.target as HTMLElement).closest("button[data-tab]") as HTMLButtonElement | null
    if (!btn || btn.hidden) return
    void renderSettingsTab(btn.dataset.tab!)
  })
  // 性能模式变更（本标签手动切换 / 其他标签 storage 同步）时刷新已打开的外观 tab 描述
  document.addEventListener("gebai:low-power-change", () => refreshAppearanceDesc())
}

/** 打开设置面板（默认「环境变量」tab；外部入口可指定 tab）。 */
export function openSettings(tab = "env") {
  settingsOverlay.hidden = false
  void renderSettingsTab(tab)
}

/** 外观 tab 当前渲染的刷新函数（事件回调用；未渲染时为 null）。 */
let appearanceRefresh: (() => void) | null = null
function refreshAppearanceDesc(): void {
  appearanceRefresh?.()
}

async function renderSettingsTab(tab: string) {
  for (const b of settingsTabs.querySelectorAll("button")) b.classList.toggle("active", b.dataset.tab === tab)
  appearanceRefresh = null // 离开外观 tab 后事件不再刷新旧描述
  settingsBody.innerHTML = ""
  // 管理员 tab（用户/反馈）按当前用户角色显示
  try {
    const me = await client.getCurrentUser()
    const admin = me.role === "admin"
    for (const b of settingsTabs.querySelectorAll<HTMLButtonElement>("button[data-tab='users'], button[data-tab='feedback']")) {
      b.hidden = !admin
    }
  } catch {
    /* 非服务模式：保持默认隐藏 */
  }
  if (tab === "env") return void renderSettingsEnv()
  if (tab === "appearance") return void renderSettingsAppearance()
  if (tab === "users") return void renderSettingsUsers()
  if (tab === "feedback") return void renderSettingsFeedback()
}

function settingsSection(title: string): HTMLElement {
  const sec = el("div", "settings-section")
  sec.appendChild(el("div", "settings-section-title", title))
  return sec
}

function renderSettingsAppearance() {
  settingsBody.appendChild(settingsSection("外观（性能模式：无 GPU/低配机器自动降级，可手动强制开启）"))
  const list = el("div", "settings-list")
  const row = el("label", "settings-row")
  const cb = document.createElement("input")
  cb.type = "checkbox"
  cb.className = "ck"
  const info = el("div", "settings-row-info")
  const desc = el("div", "settings-row-desc")
  const refreshDesc = () => {
    const forced = getLowPowerSetting() === "on" // 跨标签同步：设置可能在别的标签被修改
    const active = isLowPower()
    const noGpu = probeWebGL() ? "" : " · 未检测到 WebGL（GPU 不可用）"
    if (forced) desc.textContent = "已手动开启，低性能模式生效"
    else desc.textContent = active ? `自动检测已触发，低性能模式生效${noGpu}` : `自动检测未触发，标准模式${noGpu}`
    if (active) desc.textContent += "；降级内容：关闭动画/过渡/毛玻璃特效，图表按需渲染（点按「渲染图表」才渲染，引擎不预加载），无语言标注的代码块不做高亮自动检测，流式输出降频重渲染，视口外消息跳过渲染，图表 PNG 导出降采样"
  }
  cb.checked = getLowPowerSetting() === "on"
  cb.onchange = () => {
    setLowPowerSetting(cb.checked ? "on" : "off")
    refreshDesc() // 局部刷新说明，保持开关可用
  }
  info.append(el("div", "settings-row-name", "性能模式"), desc)
  row.append(cb, info)
  list.appendChild(row)
  settingsBody.appendChild(list)
  appearanceRefresh = () => {
    cb.checked = getLowPowerSetting() === "on"
    refreshDesc()
  }
  refreshDesc()
}

async function renderSettingsEnv() {
  settingsBody.appendChild(settingsSection("环境变量（目录来自服务端、不可自定义；保存在浏览器本地，发送消息时随请求临时注入，不落盘）"))
  let groups: EnvCatalogGroup[] = []
  try {
    groups = (await client.getEnvCatalog()).groups
  } catch {
    settingsBody.appendChild(blockText("环境变量目录获取失败（GET /api/v1/env/catalog 不可用），请确认服务端版本"))
    return
  }
  const env = loadLocalEnv()
  const catalogNames = new Set(groups.flatMap((g) => g.vars.map((v) => v.name)))
  const stale = Object.keys(env).filter((k) => !catalogNames.has(k))

  // 按「全局 / 各子Agent」分组展示；未配置的项留空，请求不携带
  for (const g of groups) {
    const det = document.createElement("details")
    det.open = true
    const sum = document.createElement("summary")
    sum.textContent = `${g.label}（${g.vars.length} 项）`
    det.appendChild(sum)
    const list = el("div", "settings-list")
    for (const v of g.vars) {
      const row = el("div", "settings-row")
      const name = el("span", "", v.name)
      name.title = v.description // tip：变量作用说明
      name.style.flex = "0 1 300px"
      name.style.fontFamily = "var(--font-mono)"
      name.style.fontSize = "12px"
      name.style.overflowWrap = "anywhere"
      const input = document.createElement("input")
      input.dataset.envName = v.name
      input.value = env[v.name] ?? ""
      input.title = v.description
      input.style.flex = "1"
      row.append(name, input)
      list.appendChild(row)
    }
    det.appendChild(list)
    settingsBody.appendChild(det)
  }

  settingsBody.appendChild(blockText("说明：仅可配置上表目录项（模型 / 子Agent / 常用变量），不可自定义变量名；未配置的项留空、请求不携带；启动级与安全敏感变量（GEBAI_MODE、GEBAI_ADMIN_PASSWORD_HASH、GEBAI_SAFE_MODE 等）不可配置。变量仅保存在本浏览器（localStorage，清除站点数据即清除），随每次发送消息临时注入服务端（仅本次任务生效，不落盘）——服务端不配置模型变量时，仅在此配置即可使用。"))

  const actions = el("div", "settings-actions")
  const saveBtn = el("button", "mini-btn", "保存")
  saveBtn.onclick = () => {
    const vars: Record<string, string> = {}
    for (const input of Array.from(settingsBody.querySelectorAll<HTMLInputElement>("input[data-env-name]"))) {
      const val = input.value.trim()
      if (val) vars[input.dataset.envName!] = val
    }
    saveLocalEnv(filterEnvToCatalog(vars, groups))
    const msg = stale.length
      ? `已保存到浏览器本地（忽略 ${stale.length} 个目录外旧变量：${stale.join("、")}）`
      : "已保存到浏览器本地（对本浏览器所有会话生效）"
    toast(msg)
  }
  actions.appendChild(saveBtn)
  settingsBody.appendChild(actions)
}

async function renderSettingsUsers() {
  settingsBody.appendChild(settingsSection("用户管理（管理员）"))
  const form = el("form", "settings-form")
  form.setAttribute("novalidate", "") // 原生校验气泡关闭，改自绘校验
  const name = document.createElement("input")
  name.placeholder = "用户名"
  const pass = document.createElement("input")
  pass.type = "password"
  pass.placeholder = "密码"
  const roleSel = customSelect({
    options: [
      { value: "user", label: "user" },
      { value: "admin", label: "admin" },
    ],
    value: "user",
    onChange: () => {},
  })
  const add = el("button", "mini-btn", "创建")
  form.append(el("div", "settings-form-label", "用户名"), name, el("div", "settings-form-label", "密码"), pass, roleSel.root, add)
  form.onsubmit = async (e) => {
    e.preventDefault()
    const uname = name.value.trim()
    if (!uname) {
      toast("请填写用户名")
      return
    }
    if (!pass.value) {
      toast("请填写密码")
      return
    }
    try {
      await client.createUser(uname, pass.value, roleSel.value as "user" | "admin")
      name.value = ""
      pass.value = ""
      await renderSettingsUsers()
    } catch (err) {
      setConn(`创建失败: ${(err as Error).message}`, false)
    }
  }
  settingsBody.appendChild(form)

  const list = el("div", "settings-list")
  let users: UserInfo[] = []
  try {
    users = (await client.listUsers()) ?? []
  } catch (err) {
    settingsBody.appendChild(blockText(`加载失败: ${(err as Error).message}`))
    return
  }
  for (const u of users) {
    const row = el("div", "settings-row")
    const info = el("div", "settings-row-info")
    const badge = u.pending ? " · 待审批" : u.disabled ? " · 已禁用" : ""
    info.append(
      el("div", "settings-row-name", `${u.username} · ${u.role}${badge}`),
      el("div", "settings-row-desc", `id: ${u.id}`),
    )
    if (u.pending) {
      // 注册待审批（GEBAI_SIGNUP_MODE=approval）：批准（启用+清除待审）/ 拒绝（删除）
      const approve = el("button", "mini-btn", "批准")
      approve.onclick = async () => {
        try {
          await client.updateUser(u.id, { disabled: false, pending: false })
          await renderSettingsUsers()
        } catch (err) {
          setConn(`操作失败: ${(err as Error).message}`, false)
        }
      }
      const reject = el("button", "mini-btn danger", "拒绝")
      reject.onclick = async () => {
        if (!(await confirmDialog({ title: "拒绝注册", text: `拒绝并删除待审批账号 ${u.username}？` }))) return
        try {
          await client.deleteUser(u.id)
          await renderSettingsUsers()
        } catch (err) {
          setConn(`操作失败: ${(err as Error).message}`, false)
        }
      }
      row.append(info, approve, reject)
      list.appendChild(row)
      continue
    }
    const toggle = el("button", "mini-btn", u.disabled ? "启用" : "禁用")
    toggle.onclick = async () => {
      try {
        await client.updateUser(u.id, { disabled: !u.disabled })
        await renderSettingsUsers()
      } catch (err) {
        setConn(`操作失败: ${(err as Error).message}`, false)
      }
    }
    const del = el("button", "mini-btn danger", "删除")
    del.onclick = async () => {
      if (!(await confirmDialog({ title: "删除用户", text: `删除用户 ${u.username}？` }))) return
      try {
        await client.deleteUser(u.id)
        await renderSettingsUsers()
      } catch (err) {
        setConn(`删除失败: ${(err as Error).message}`, false)
      }
    }
    row.append(info, toggle, del)
    list.appendChild(row)
  }
  settingsBody.appendChild(list)
}

async function renderSettingsFeedback() {
  settingsBody.appendChild(settingsSection("反馈列表（管理员）"))
  const list = el("div", "settings-list")
  let feedback: FeedbackInfo[] = []
  try {
    feedback = (await client.listFeedback()) ?? []
  } catch (err) {
    settingsBody.appendChild(blockText(`加载失败: ${(err as Error).message}`))
    return
  }
  for (const f of feedback) {
    const row = el("div", "settings-row")
    const info = el("div", "settings-row-info")
    info.append(
      el("div", "settings-row-name", `${f.type}${f.label ? ` · ${f.label}` : ""} · ${new Date(f.createdAt).toLocaleString()}`),
      el("div", "settings-row-desc", `用户 ${f.userId} · 会话 ${f.sessionId?.slice(0, 8)} · 消息 ${f.messageId?.slice(0, 8)}${f.text ? `\n${f.text}` : ""}`),
    )
    row.appendChild(info)
    list.appendChild(row)
  }
  if (!feedback.length) list.appendChild(blockText("（暂无反馈）"))
  settingsBody.appendChild(list)
}
