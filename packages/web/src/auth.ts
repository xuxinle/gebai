import { appBase, clearTokenState, readToken, requestHeaders, writeToken } from "@gebai/sdk"
import { client, loginErr, loginForm, loginOverlay, loginPass, loginPass2, loginSubmit, loginToggle, loginUser, logoutBtn, runs, sessionList, setCurrentSession } from "./state"
import { loadMessages, refreshSessions, enterDraftView, resetMsgWindow } from "./sessions"
import { toast } from "./ui"
import { parseExternalCredential } from "./external-auth"
import { signupUiAllowed } from "./boot-config"

/* ---------- 认证（服务模式） ---------- */

const AUTH_TOKEN_KEY = "gebai.auth.token"
/** 令牌 cookie 有效期：与服务端令牌 TTL 一致（packages/server/src/auth.ts，7 天）。 */
const AUTH_TOKEN_COOKIE_MAX_AGE = 7 * 24 * 3600

/**
 * 把登录态同步到同源 cookie（与令牌同一份值、同一键名）。
 *
 * 为什么需要：页面里的图片/视频/iframe/下载是**浏览器原生请求**（`<img src>`/`<video src>`/
 * `<a download>`），由浏览器自行发出，前端脚本没有插入请求头的机会——只存本地存储的令牌带不上去，
 * 服务模式下消息流里的图片一律 401。写一份同名 cookie 后这类请求自动带上凭证（服务端
 * credential-sources 的 cookie 来源校验同一令牌，且**只对 GET/HEAD 生效**，写端点仍只认 Bearer）。
 *
 * **不写空值**：`token` 为空时直接返回——未登录实例的页面加载**不得**清掉同 host 另一个实例的
 * cookie（cookie 按 host 共享、不隔离端口，同 IP 多实例会互相看见）；清 cookie 只发生在显式登出
 * （`clearTokenCookie`）。部署方自定义了非 Bearer 载体（`window.__GEBAI_AUTH__.requestHeaders`）时
 * 同样不写——载体选择是部署方的决定，框架不自作主张增开通道。
 */
export function syncTokenCookie(token: string) {
  if (!token) return
  try {
    if (!requestHeaders(token).Authorization) return
    const path = appBase() || "/"
    document.cookie = `${AUTH_TOKEN_KEY}=${encodeURIComponent(token)}; path=${path}; max-age=${AUTH_TOKEN_COOKIE_MAX_AGE}; SameSite=Lax`
  } catch {
    /* 忽略：cookie 不可写（隐私模式等）时退化为仅请求头通道 */
  }
}

/** 清除令牌 cookie（显式登出）。 */
export function clearTokenCookie() {
  try {
    const path = appBase() || "/"
    document.cookie = `${AUTH_TOKEN_KEY}=; path=${path}; max-age=0`
  } catch {
    /* 忽略 */
  }
}

/**
 * 恢复本地持久化的登录态（服务模式）。
 * 令牌的存放位置由**凭证契约**决定（默认 localStorage `gebai.auth.token`，见 @gebai/sdk
 * auth-contract）——部署方可用 `window.__GEBAI_AUTH__` 换成自己的载体（如 sessionStorage、
 * 宿主系统登录态），此处与登录/登出一并改走契约，保证「存哪」与「读哪」始终一致。
 */
export function restoreToken() {
  const t = readToken()
  if (t) {
    client.setToken(t)
    // 已登录直进聊天页（不经 showLogin）：登出按钮同步显示
    logoutBtn.hidden = false
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
    writeToken(client.getToken() ?? "")
  } catch {
    /* 忽略 */
  }
  syncTokenCookie(client.getToken() ?? "")
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
    clearTokenState()
  } catch {
    /* 忽略 */
  }
  clearTokenCookie() // 登出显式清 cookie：原生资源请求不再带任何凭证
  setCurrentSession(null)
  runs.clear()
  resetMsgWindow()
  sessionList.innerHTML = ""
  showLogin()
}

export function bindAuth() {
  logoutBtn.onclick = () => void doLogout()

  // 登录 / 注册模式切换（注册仅服务模式开放；注册用户恒为普通角色，admin 只能由部署方配置哈希启用）。
  // 注册入口可被两层禁用（任一生效即隐藏，注册只能由二开前端代码发起）：
  //   ① 二开配置 allowSignup:false（gebai.config.js / __GEBAI_WEB_CONFIG__，纯前端决策）
  //   ② 服务端 GEBAI_SIGNUP_SOURCE=custom（探测端点 builtinAllowed=false，且服务端对无来源头注册 403 兜底）
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

  // 两层禁用闸门（先声明后使用，异步探测回调与本地配置同步调用共用）：任一生效即隐藏切换按钮并
  // 回到登录模式；loginOverlay 已展示时在错误栏提示原因（未展示则只隐藏，不主动弹登录页）
  let signupAllowed = signupUiAllowed()
  const disableSignup = (reason: string) => {
    if (!signupAllowed) return
    signupAllowed = false
    loginToggle.hidden = true
    setMode(false)
    if (!loginOverlay.hidden) {
      loginErr.textContent = reason
      loginErr.hidden = false
    }
  }
  if (!signupAllowed) disableSignup("本部署未开放页面注册")
  client
    .getSignupConfig()
    .then((cfg) => {
      if (cfg.builtinAllowed === false) disableSignup("本部署未开放页面注册，请联系管理员或宿主系统")
    })
    .catch(() => {
      /* 探测失败（本地模式/网络异常）：保持本地配置决策，不阻塞登录页 */
    })

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
        writeToken(client.getToken() ?? "")
      } catch {
        /* 忽略 */
      }
      syncTokenCookie(client.getToken() ?? "")
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
