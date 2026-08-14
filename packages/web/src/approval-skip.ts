import { client, getCurrentSession } from "./state"
import { tip } from "./ui"

/* ---------- 自动审批：跳过工具调用审批，持久化到本地 ---------- */

const APPROVAL_KEY = "gebai.approval.skip"
const approvalSkipBtn = document.getElementById("approval-skip")!

function approvalSkipEnabled(): boolean {
  try {
    return localStorage.getItem(APPROVAL_KEY) === "1"
  } catch {
    return false
  }
}

function syncApprovalSkipBtn() {
  const on = approvalSkipEnabled()
  approvalSkipBtn.classList.toggle("active", on)
  tip(approvalSkipBtn, on ? "自动审批已开启：工具调用不再请求审批" : "自动审批：跳过工具调用审批（持久化到本地）")
}

/** 当前会话同步自动审批开关（幂等；会话 env 持久化在服务端）。 */
export async function applyApprovalSkip(sessionId: string) {
  if (!approvalSkipEnabled() || !sessionId) return
  try {
    await client.setSessionEnv(sessionId, { GEBAI_APPROVAL_SKIP: "true" })
  } catch {
    /* 忽略设置失败 */
  }
}

export function bindApprovalSkip() {
  syncApprovalSkipBtn()
  approvalSkipBtn.onclick = async () => {
    const enabled = !approvalSkipEnabled()
    try {
      localStorage.setItem(APPROVAL_KEY, enabled ? "1" : "0")
    } catch {
      /* 忽略 */
    }
    syncApprovalSkipBtn()
    const cur = getCurrentSession()
    if (cur) {
      try {
        await client.setSessionEnv(cur.id, enabled ? { GEBAI_APPROVAL_SKIP: "true" } : { GEBAI_APPROVAL_SKIP: null })
      } catch {
        /* 忽略 */
      }
      if (enabled) {
        // 会话运行中开启：当前等待中的审批卡片立即自动通过（引擎审批点实时判定，后续审批同样跳过）
        for (const card of document.querySelectorAll<HTMLElement>(".approval")) {
          if (card.dataset.session === cur.id) card.querySelector<HTMLButtonElement>(".yes")?.click()
        }
      }
    }
  }
}
