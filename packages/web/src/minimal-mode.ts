import { client, getCurrentSession } from "./state"
import { tip, toast } from "./ui"

/* ---------- 极简模式：会话仅启用 sh 与 edit 工具，持久化到本地 ---------- */

const MINIMAL_KEY = "gebai.minimal.mode"
const minimalModeBtn = document.getElementById("minimal-mode")!

function minimalModeEnabled(): boolean {
  try {
    return localStorage.getItem(MINIMAL_KEY) === "1"
  } catch {
    return false
  }
}

function syncMinimalModeBtn() {
  const on = minimalModeEnabled()
  minimalModeBtn.classList.toggle("active", on)
  tip(minimalModeBtn, on ? "极简模式已开启：会话仅启用 sh 与 edit 工具" : "极简模式：会话仅启用 sh 与 edit 工具（持久化到本地）")
}

/** 当前会话同步极简模式开关（幂等）。开关本身持久化在浏览器本地（localStorage 为准）；
 *  会话 env 仅写入服务端内存（不落盘，服务端零留存），任务启动时按 env 快照裁剪工具集（下次任务起生效），
 *  重启后由此处重新同步。 */
export async function applyMinimalMode(sessionId: string) {
  if (!sessionId) return
  try {
    await client.setSessionEnv(sessionId, { GEBAI_MINIMAL_MODE: minimalModeEnabled() ? "true" : null })
  } catch (e) {
    toast(`极简模式同步失败: ${e instanceof Error ? e.message : String(e)}`)
  }
}

export function bindMinimalMode() {
  syncMinimalModeBtn()
  minimalModeBtn.onclick = async () => {
    const enabled = !minimalModeEnabled()
    try {
      localStorage.setItem(MINIMAL_KEY, enabled ? "1" : "0")
    } catch {
      /* 忽略 */
    }
    syncMinimalModeBtn()
    const cur = getCurrentSession()
    if (cur) {
      try {
        await client.setSessionEnv(cur.id, { GEBAI_MINIMAL_MODE: enabled ? "true" : null })
      } catch (e) {
        toast(`极简模式设置失败: ${e instanceof Error ? e.message : String(e)}`)
      }
    }
  }
}
