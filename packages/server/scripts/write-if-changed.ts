/**
 * 内容不变不写盘（构建脚本产物写入统一入口）。
 *
 * 背景：typecheck/build 链每次都重跑全部 build-*.ts 生成脚本，无条件 writeFileSync 会把
 * 已提交的生成产物（如 cvdriver.embedded.generated.json）与 gitignore 产物的 mtime 一并刷新，
 * 造成「跑一次 typecheck 工作区就变脏」的噪声（AI 每轮都要 stash 回退确认是否既有改动）。
 * 内容一致时跳过写入，工作区与 mtime 保持稳定；内容变化（真的改了源码/依赖）照常落盘。
 */
import { readFileSync, writeFileSync } from "node:fs"

/** 写入文件；与磁盘现有内容逐字节相同则跳过（返回 false）。 */
export function writeFileIfChanged(path: string, content: string | Uint8Array): boolean {
  const next = typeof content === "string" ? Buffer.from(content, "utf8") : Buffer.from(content)
  try {
    if (Buffer.compare(readFileSync(path), next) === 0) return false
  } catch {
    /* 文件不存在/不可读：照常写入 */
  }
  writeFileSync(path, next)
  return true
}
