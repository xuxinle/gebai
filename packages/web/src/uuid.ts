/**
 * UUID v4 生成：优先 crypto.randomUUID()（安全上下文）；
 * 纯 http://<IP>:3000 远程访问属于非安全上下文，randomUUID 不存在，回退到 Math.random 实现。
 */

function fallbackUuid(): string {
  return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, (c) => {
    const r = (Math.random() * 16) | 0
    const v = c === "x" ? r : (r & 0x3) | 0x8
    return v.toString(16)
  })
}

export function uuid(): string {
  return typeof crypto !== "undefined" && typeof crypto.randomUUID === "function"
    ? crypto.randomUUID()
    : fallbackUuid()
}
