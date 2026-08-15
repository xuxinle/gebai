import { client, loginErr, loginForm, loginOverlay, loginPass, loginPass2, loginSubmit, loginToggle, loginUser, logoutBtn, msgEl, runs, sessionList, setCurrentSession } from "./state"
import { loadMessages, refreshSessions, enterDraftView } from "./sessions"
import { toast } from "./ui"
import { parseExternalCredential } from "./external-auth"

/* ---------- 认证（服务模式） ---------- */

const AUTH_TOKEN_KEY = "gebai.auth.token"

/** 恢复本地持久化的登录态（服务模式）。 */
export function restoreToken() {
  try {
    const t = localStorage.getItem(AUTH_TOKEN_KEY)
    if (t) {
      client.setToken(t)
      // 已登录直进聊天页（不经 showLogin）：登出按钮同步显示
      logoutBtn.hidden = false
    }
  } catch {
    /* 忽略 */
  }
}

/**
 * 外部身份兑换：配置了外部身份扩展点且本地无令牌时，用 URL 参数或宿主 localStorage 里的
 * 外部登录态兑换歌白令牌。成功返回 true（令牌已写入 client 与 localStorage），失败/未启用 false。
 */
export async function tryExternalAuth(): Promise<boolean> {
  if (client.getToken()) return false
  let cfg: { enabled: boolean; storageKey?: string | null }
  try {
    cfg = await client.getExternalAuthConfig()
  } catch {
    return false
  }
  if (!cfg.enabled) return false
  const cred = parseExternalCredential(new URLSearchParams(location.search), localStorage, cfg.storageKey ?? null)
  if (!cred) return false
  try {
    await client.exchangeExternalUser(cred.username, cred.credential)
  } catch {
    return false // 凭证无效：回落正常登录页
  }
  // 兑换成功即从地址栏移除凭证参数（防滞留 URL 历史/Referer/服务端访问日志），保留其余参数
  try {
    const params = new URLSearchParams(location.search)
    params.delete("gb_ext_username")
    params.delete("gb_ext_credential")
    const qs = params.toString()
    history.replaceState(null, "", qs ? `${location.pathname}?${qs}` : location.pathname)
  } catch {
    /* 忽略 */
  }
  try {
    localStorage.setItem(AUTH_TOKEN_KEY, client.getToken() ?? "")
  } catch {
    /* 忽略 */
  }
  logoutBtn.hidden = false
  return true
}

export function showLogin(reason?: string) {
  loginOverlay.hidden = false
  logoutBtn.hidden = false
  if (reason) {
    loginErr.textContent = reason
    loginErr.hidden = false
  }
  loginUser.focus()
}

function hideLogin() {
  loginOverlay.hidden = true
  loginErr.hidden = true
}

export async function doLogout() {
  try {
    await client.logout()
  } catch {
    /* 忽略 */
  }
  try {
    localStorage.removeItem(AUTH_TOKEN_KEY)
  } catch {
    /* 忽略 */
  }
  setCurrentSession(null)
  runs.clear()
  msgEl.innerHTML = ""
  sessionList.innerHTML = ""
  showLogin()
}

export function bindAuth() {
  logoutBtn.onclick = () => void doLogout()

  // 登录 / 注册模式切换（注册仅服务模式开放；注册用户恒为普通角色，admin 只能由部署方配置哈希启用）
  let regMode = false
  const setMode = (reg: boolean) => {
    regMode = reg
    loginPass2.hidden = !reg
    loginSubmit.textContent = reg ? "注 册" : "登 录"
    loginToggle.textContent = reg ? "已有账号？去登录" : "注册账号"
    loginUser.placeholder = reg ? "用户名（注册即登录）" : "用户名"
    loginPass.autocomplete = reg ? "new-password" : "current-password"
    loginErr.hidden = true
    loginUser.focus()
  }
  loginToggle.onclick = () => setMode(!regMode)

  loginForm.addEventListener("submit", async (e) => {
    e.preventDefault()
    loginErr.hidden = true
    // 自绘校验（novalidate 已关闭原生气泡）
    if (!loginUser.value.trim() || !loginPass.value) {
      toast("请填写用户名和密码")
      return
    }
    try {
      if (regMode) {
        if (loginPass.value !== loginPass2.value) {
          toast("两次输入的密码不一致")
          return
        }
        const r = await client.register(loginUser.value.trim(), loginPass.value)
        if (r.pending) {
          // 审批模式：注册成功但待 admin 审批（不可登录），提示后回到登录态
          loginPass.value = ""
          loginPass2.value = ""
          setMode(false)
          loginErr.textContent = `注册成功，账号「${r.user.username}」待管理员审批，通过后可登录`
          loginErr.hidden = false
          return
        }
      } else {
        await client.login(loginUser.value.trim(), loginPass.value)
      }
      try {
        localStorage.setItem(AUTH_TOKEN_KEY, client.getToken() ?? "")
      } catch {
        /* 忽略 */
      }
      hideLogin()
      await refreshSessions()
      const sessions = await client.listSessions()
      // 登录后进入最近会话；无会话则进入空白草稿页（首条消息发送时才创建，与主流程一致）
      if (sessions.length) {
        setCurrentSession(sessions[0])
        void refreshSessions(sessions)
        await loadMessages(sessions[0].id)
      } else {
        enterDraftView()
      }
    } catch (err) {
      loginErr.textContent = (err as Error).message
      loginErr.hidden = false
    }
  })
}
