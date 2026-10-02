/**
 * 会话导入/导出（纯逻辑，可测；DOM 绑定在 sessions.ts）。
 *
 * **格式「批量包」（version 1）**：单个与批量统一为 `SessionsExportFile`——单个会话即 `sessions`
 * 数组只有一项，导入侧同一解析器通吃单/批量/多文件（文件选择器 multiple + 单文件多会话都覆盖）。
 * 只携带 `name/createdAt/updatedAt/pinned/messages/todos/loadedSubAgents`；**不携带 id/userId/ctx
 * 统计**——导入时服务端分配新 id、导入者成为 owner（`SessionStore.parseImport`），跨实例导出/导入
 * 不存在 id 冲突，也不给「指名注入某会话 id」开口子。
 *
 * 与轮盘既有的「导出会话」（Markdown 阅读版，`exportSession`）并存：那是给人看的，本文件是
 * 给歌白自己吃的（往返无损：Message 全字段——blocks/toolCalls/reasoning/subSessionArchive——
 * 原样进 JSON，导入后回放渲染与原会话一致）。
 */
import type { SessionDetail, SessionImportData, SessionsExportFile } from "@gebai/sdk"

/** 导出文件格式标识与版本（与 `SessionsExportFile` 对齐）。 */
export const SESSION_EXPORT_FORMAT = "gebai-sessions"
export const SESSION_EXPORT_VERSION = 1

/** 单个导出包的会话条数上限（防御：批量导出/导入的内存与网络体量护栏）。 */
export const SESSION_EXPORT_MAX_SESSIONS = 200

/** 会话详情 → 导出载荷条目（剥离 id/userId/ctx 统计；messages/todos/loadedSubAgents 原样保留）。 */
export function detailToImportData(detail: SessionDetail): SessionImportData {
  const out: SessionImportData = {
    name: detail.name || "未命名会话",
    createdAt: detail.createdAt,
    updatedAt: detail.updatedAt,
    pinned: detail.pinned === true,
    messages: detail.messages ?? [],
  }
  if (detail.todos?.length) out.todos = detail.todos
  if (detail.loadedSubAgents?.length) out.loadedSubAgents = detail.loadedSubAgents
  return out
}

/** 打包导出文件对象（单/批量统一）。 */
export function buildExportFile(sessions: SessionImportData[]): SessionsExportFile {
  if (!sessions.length) throw new Error("没有可导出的会话")
  if (sessions.length > SESSION_EXPORT_MAX_SESSIONS) throw new Error(`单次导出最多 ${SESSION_EXPORT_MAX_SESSIONS} 个会话`)
  return { format: SESSION_EXPORT_FORMAT, version: SESSION_EXPORT_VERSION, exportedAt: Date.now(), sessions }
}

/** 解析导出文件文本（JSON）：格式/版本不符给可读错误，会话条目逐个基础校验（服务端为最权威校验）。
 *  返回会话载荷数组（空数组视为错误——导出文件不会是空的）。 */
export function parseImportText(text: string): SessionImportData[] {
  let raw: unknown
  try {
    raw = JSON.parse(text)
  } catch {
    throw new Error("不是有效的 JSON 文件")
  }
  if (typeof raw !== "object" || raw === null) throw new Error("文件格式不符（应为歌白会话导出文件）")
  const f = raw as Partial<SessionsExportFile>
  if (f.format !== SESSION_EXPORT_FORMAT) throw new Error("文件格式不符（应为歌白会话导出文件）")
  if (f.version !== SESSION_EXPORT_VERSION) throw new Error(`不支持的导出版本: ${String(f.version)}（当前支持 v${SESSION_EXPORT_VERSION}）`)
  if (!Array.isArray(f.sessions) || f.sessions.length === 0) throw new Error("导出文件中没有会话")
  if (f.sessions.length > SESSION_EXPORT_MAX_SESSIONS) throw new Error(`单次导入最多 ${SESSION_EXPORT_MAX_SESSIONS} 个会话`)
  for (const s of f.sessions) {
    if (typeof s !== "object" || s === null || typeof (s as SessionImportData).name !== "string" || !(s as SessionImportData).name.trim()) {
      throw new Error("导出文件里有会话缺少名称（文件可能已损坏）")
    }
  }
  return f.sessions
}

/** 文件名消毒（与 exportSession 的 Markdown 导出同规则：非法字符替换、首尾点空格去除、长度截断）。 */
export function safeFileStem(name: string, fallback: string): string {
  return (
    name
      .replace(/[\\/:*?"<>|\x00-\x1f\x7f]/g, "_")
      .trim()
      .replace(/^[.\s]+|[.\s]+$/g, "")
      .slice(0, 80) || fallback
  )
}

/** 单会话导出文件名：`gebai-session-{名称}.json`。 */
export function singleExportFileName(name: string): string {
  return `gebai-session-${safeFileStem(name, "未命名")}.json`
}

/** 批量导出文件名：`gebai-sessions-{N}会话-{yyyymmdd}.json`。 */
export function batchExportFileName(count: number): string {
  const d = new Date()
  const ymd = `${d.getFullYear()}${String(d.getMonth() + 1).padStart(2, "0")}${String(d.getDate()).padStart(2, "0")}`
  return `gebai-sessions-${count}会话-${ymd}.json`
}

/** 下载 JSON 文本为文件（与 exportSession 的下载路径同构：Blob + 临时 a + 延迟回收）。 */
export function downloadJson(fileName: string, data: unknown): void {
  const blob = new Blob([JSON.stringify(data, null, 2)], { type: "application/json;charset=utf-8" })
  const url = URL.createObjectURL(blob)
  const a = document.createElement("a")
  a.href = url
  a.download = fileName
  a.click()
  setTimeout(() => URL.revokeObjectURL(url), 1000)
}
