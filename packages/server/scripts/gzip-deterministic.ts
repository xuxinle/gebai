/**
 * 确定性 gzip（构建内嵌产物共用）。
 *
 * 背景：内嵌产物 `*.embedded.generated.json` 以 gzip base64 保存，而 zlib 的 gzip 头携带
 * **平台相关字节**（XFL/OS）——同一份源码在 Windows 与 Linux 上生成不同字节，导致已提交的
 * 生成产物（如 cvdriver.embedded.generated.json）在另一平台构建后工作区变脏、无法判定
 * 「是源码改动还是平台差异」。此处把头部 XFL/OS 固定为常量，使产物跨平台逐字节一致。
 *
 * 另：gzip 头部 mtime 由 zlib 置 0（Bun/Node 默认），无需处理。
 */
import { gzipSync } from "node:zlib"

/** 平台无关 gzip：压缩后把头部 XFL(8)/OS(9) 固定为 0x00/0x03（Unix），跨平台字节一致。 */
export function gzipDeterministic(data: Uint8Array): Buffer {
  const out = Buffer.from(gzipSync(data))
  out[8] = 0x00
  out[9] = 0x03
  return out
}
