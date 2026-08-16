import { attachmentsEl, client, el, getCurrentSession, pendingFiles, pendingFilesBySession, setPendingFiles } from "./state"
import { uuid } from "./uuid"
import { appendMsg } from "./messages"
import { syncSendButton } from "./composer"

/* ---------- 附件 ---------- */

/** 图片压缩上限（DESIGN 常量参考）：长边或体积超限时 canvas 重采样。 */
const IMG_MAX_EDGE = 1280
const IMG_MAX_BYTES = 2 * 1024 * 1024
const IMG_QUALITY = 0.85

function loadImage(f: File): Promise<HTMLImageElement> {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(f)
    const img = new Image()
    img.onload = () => {
      URL.revokeObjectURL(url)
      resolve(img)
    }
    img.onerror = () => {
      URL.revokeObjectURL(url)
      reject(new Error("图片解码失败"))
    }
    img.src = url
  })
}

function canvasToBlob(canvas: HTMLCanvasElement, mime: string): Promise<Blob | null> {
  return new Promise((resolve) => canvas.toBlob(resolve, mime, IMG_QUALITY))
}

/**
 * 图片自动压缩重采样（DESIGN「多模态支持」）：PNG/JPEG 长边 >1280px 或体积 >2MB 时
 * canvas 缩放重编码；PNG 优先保留透明通道（仅当缩放后体积仍超限才转 JPEG，白色底）；
 * 转 JPEG 时同步修正扩展名（vision 工具按扩展名判 MIME）。GIF 等跳过。
 */
async function compressImage(f: File): Promise<File> {
  if (!["image/png", "image/jpeg"].includes(f.type)) return f
  let img: HTMLImageElement
  try {
    img = await loadImage(f)
  } catch {
    return f
  }
  const scale = Math.min(1, IMG_MAX_EDGE / Math.max(img.width, img.height))
  if (scale >= 1 && f.size <= IMG_MAX_BYTES) return f
  const w = Math.max(1, Math.round(img.width * scale))
  const h = Math.max(1, Math.round(img.height * scale))
  const canvas = document.createElement("canvas")
  canvas.width = w
  canvas.height = h
  const ctx = canvas.getContext("2d")
  if (!ctx) return f
  let outMime = f.type
  if (outMime === "image/jpeg") ctx.fillStyle = "#fff"
  if (outMime === "image/jpeg") ctx.fillRect(0, 0, w, h)
  ctx.drawImage(img, 0, 0, w, h)
  let blob = await canvasToBlob(canvas, outMime)
  // PNG 缩放后仍超限才转 JPEG（透明通道丢失，白底兜底）
  if (outMime === "image/png" && (!blob || blob.size > IMG_MAX_BYTES)) {
    const jpg = await canvasToBlob(canvas, "image/jpeg")
    if (jpg) {
      blob = jpg
      outMime = "image/jpeg"
    }
  }
  if (!blob || blob.size >= f.size) return f
  const name = outMime === f.type ? f.name : f.name.replace(/\.(png|jpe?g)$/i, outMime === "image/jpeg" ? ".jpg" : ".png")
  return new File([blob], name, { type: outMime })
}

export async function addPendingFiles(files: FileList | File[]) {
  let added = 0
  for (const f of Array.from(files)) {
    const ef = await compressImage(f)
    if (pendingFiles.some((p) => p.name === ef.name && p.size === ef.size)) continue
    setPendingFiles([...pendingFiles, { name: ef.name, mime: ef.type || "application/octet-stream", size: ef.size, blob: ef }])
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
