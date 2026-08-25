import type { FeedbackInfo, UserInfo } from "@gebai/sdk"
import { client, el, setConn, settingsBody, settingsBtn, settingsFoot, settingsOverlay, settingsTabs } from "./state"
import { blockText } from "./markdown"
import { isLowPower, setLowPowerSetting } from "./low-power"
import { isTurnTimerEnabled, setTurnTimerSetting } from "./turn-timer"
import { isFilePopup, setFileDisplaySetting } from "./file-display"
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
  settingsFoot.hidden = true // 底部固定区仅环境变量 tab 使用
  settingsFoot.innerHTML = ""
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
  const list = el("div", "settings-list")

  // 低性能模式
  const row = el("div", "settings-row")
  const info = el("div", "settings-row-info")
  const desc = el("div", "settings-row-desc")
  const btn = el("button", "mini-btn")
  const refreshDesc = () => {
    const on = isLowPower() // 跨标签同步：设置可能在别的标签被修改
    btn.textContent = on ? "关闭" : "开启"
    desc.textContent = on ? "已开启：关闭动画等特效" : "未开启：使用完整动画与特效"
  }
  btn.onclick = () => {
    setLowPowerSetting(isLowPower() ? "off" : "on")
    refreshDesc() // 局部刷新说明，保持按钮可用
  }
  info.append(el("div", "settings-row-name", "低性能模式"), desc)
  row.append(info, btn)
  list.appendChild(row)

  // 单轮计时器：任务运行期间在流式消息上显示本轮耗时，结束定格
  const ttRow = el("div", "settings-row")
  const ttInfo = el("div", "settings-row-info")
  const ttDesc = el("div", "settings-row-desc")
  const ttBtn = el("button", "mini-btn")
  const ttRefreshDesc = () => {
    const on = isTurnTimerEnabled() // 跨标签同步：设置可能在别的标签被修改
    ttBtn.textContent = on ? "关闭" : "开启"
    ttDesc.textContent = on ? "已开启：消息上显示本轮任务耗时" : "已关闭：不显示任务耗时"
  }
  ttBtn.onclick = () => {
    setTurnTimerSetting(isTurnTimerEnabled() ? "off" : "on")
    ttRefreshDesc()
  }
  ttInfo.append(el("div", "settings-row-name", "单轮计时器"), ttDesc)
  ttRow.append(ttInfo, ttBtn)
  list.appendChild(ttRow)

  // 文件展示方式：read/write/edit/patch 等文件工具（含 code 子Agent 同款工具）的**产物文件卡**——
  // 嵌入（现状：卡片内联展示文件内容）或弹窗（收敛为文件链接，点击弹窗查看；适配会话相对与项目路径）；
  // 参数区与输出不受影响
  const fdRow = el("div", "settings-row")
  const fdInfo = el("div", "settings-row-info")
  const fdDesc = el("div", "settings-row-desc")
  const fdBtn = el("button", "mini-btn")
  const fdRefreshDesc = () => {
    const popup = isFilePopup()
    fdBtn.textContent = popup ? "改为嵌入展示" : "改为弹窗查看"
    fdDesc.textContent = popup
      ? "弹窗查看：read/write 等文件工具的产物文件卡收敛为文件链接，点击弹窗查看内容（当前会话立即生效）"
      : "嵌入展示：read/write 等文件工具的产物文件卡内联展示内容（现状）"
  }
  fdBtn.onclick = () => {
    setFileDisplaySetting(isFilePopup() ? "inline" : "popup")
    fdRefreshDesc()
  }
  fdInfo.append(el("div", "settings-row-name", "文件展示方式"), fdDesc)
  fdRow.append(fdInfo, fdBtn)
  list.appendChild(fdRow)

  settingsBody.appendChild(list)
  appearanceRefresh = refreshDesc
  refreshDesc()
  ttRefreshDesc()
  fdRefreshDesc()
}

async function renderSettingsEnv() {
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

  // 说明 + 保存按钮固定在面板底部，不随变量列表滚动
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
    toast(msg, "ok")
  }
  settingsFoot.append(el("div", "settings-foot-desc", "仅保存在本浏览器，对所有会话生效"), saveBtn)
  settingsFoot.hidden = false
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
