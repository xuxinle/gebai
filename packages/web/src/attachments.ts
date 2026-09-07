import { attachmentsEl, client, el, getCurrentSession, pendingFiles, pendingFilesBySession, setPendingFiles } from "./state"
import { uuid } from "./uuid"
import { appendMsg } from "./messages"
import { syncSendButton } from "./composer"

/* ---------- 附件 ---------- */

/** 图片按原图保存（会话 tmp/ 留无损原图供模型/工具/用户取用）；发送给大模型前的压缩在服务端进行。 */
export async function addPendingFiles(files: FileList | File[]) {
  let added = 0
  for (const f of Array.from(files)) {
    if (pendingFiles.some((p) => p.name === f.name && p.size === f.size)) continue
    setPendingFiles([...pendingFiles, { name: f.name, mime: f.type || "application/octet-stream", size: f.size, blob: f }])
    added++
  }
  if (added) renderAttachments()
}

function formatSize(n: number): string {
  if (n < 1024) return `${n} B`
  if (n < 1048576) return `${(n / 1024).toFixed(1)} KB`
  return `${(n / 1048576).toFixed(1)} MB`
}

function renderAttachments() {
  attachmentsEl.innerHTML = ""
  attachmentsEl.hidden = pendingFiles.length === 0
  for (const [i, f] of pendingFiles.entries()) {
    const chip = el("div", "attach-chip")
    chip.append(el("span", "chip-name", f.name), el("span", "chip-size", formatSize(f.size)))
    const rm = el("button", undefined, "✕")
    rm.onclick = () => {
      setPendingFiles(pendingFiles.filter((_, idx) => idx !== i))
      renderAttachments()
    }
    chip.appendChild(rm)
    attachmentsEl.appendChild(chip)
  }
  // 附件增减影响发送按钮形态（运行中有草稿=排队发送，无草稿=停止）
  syncSendButton()
}

/** 附件列表重渲染（sessions 切换恢复时调用；当前会话的附件由本模块内部维护）。 */
export { renderAttachments }

/** 上传待发送附件（绑定目标会话，返回引用列表；失败消息按会话归属渲染）。 */
export async function sendPending(sessionId: string): Promise<Array<{ name: string; mime: string; path: string }>> {
  if (!pendingFiles.length) return []
  const attachments = []
  for (const f of pendingFiles) {
    try {
      const info = await client.uploadAttachment(sessionId, f.blob, f.name)
      attachments.push({ name: f.name, mime: f.mime, path: info.path })
    } catch (err) {
      // 失败提示仅渲染到目标会话视图（若已切走则不显示，避免串台）
      if (getCurrentSession()?.id === sessionId) {
        appendMsg({ id: uuid(), role: "tool", content: `附件上传失败: ${(err as Error).message}`, createdAt: Date.now() })
      }
    }
  }
  // 目标会话附件已消费；若期间未切换会话，清空当前展示列表（切走了则保留新会话的附件）
  pendingFilesBySession.delete(sessionId)
  if (getCurrentSession()?.id === sessionId) {
    setPendingFiles([])
    renderAttachments()
  }
  return attachments
}
