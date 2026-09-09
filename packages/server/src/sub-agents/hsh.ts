/**
 * hsh 的 TS 侧贡献（跨语言合并示例，DESIGN「多语言子代理」）：description/systemPrompt 留空——
 * 能力描述与工作流提示词由 native 侧（native-agents/rust/hsh/）单独贡献，本文件只补充一个
 * 基础工具 crc32（适合 TS 直接实现的轻量逻辑），两侧经 SubAgentManager 合并为同一子代理。
 * 这是「基础工具 TS 写、特殊工具其他语言写」分工约定的落地样例。
 */
import type { SubAgentDef, Tool } from "../core/base/types"

/** CRC-32（IEEE 802.3 多项式 0xEDB88320，反射式）：查表法，hex 输出。 */
export const crc32Tool: Tool = {
  name: "crc32",
  description: "CRC-32 校验（IEEE 802.3，hex 小写）：text 或 bytes_hex 输入，data.digest 为 8 位十六进制；速度快，适用非密码学完整性场景（传输校验/去重键）",
  parameters: {
    type: "object",
    properties: {
      text: { type: "string", description: "文本输入（UTF-8 编码后计算）" },
      bytes_hex: { type: "string", description: "字节输入（hex 字符串，与 text 二选一）" },
    },
  },
  async execute(args) {
    let bytes: Uint8Array
    const text = typeof args.text === "string" ? args.text : ""
    const hex = typeof args.bytes_hex === "string" ? args.bytes_hex.trim() : ""
    if (hex) bytes = new Uint8Array((hex.match(/.{1,2}/g) ?? []).map((b) => parseInt(b, 16) || 0))
    else bytes = new TextEncoder().encode(text)
    let crc = 0xffffffff
    for (const b of bytes) {
      crc ^= b
      for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1))
    }
    const digest = ((crc ^ 0xffffffff) >>> 0).toString(16).padStart(8, "0")
    return { output: `CRC-32: ${digest}（IEEE 802.3，${bytes.length} 字节）`, data: { digest } }
  },
}

export const name = "hsh"
export const description = "" // 留空：由 native 侧贡献（跨语言合并约定——只在一处定义）
export const systemPrompt = "" // 留空：同上
export const tools = { crc32: crc32Tool }

export const def: SubAgentDef = { name, description, systemPrompt, tools }
