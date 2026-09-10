/**
 * 运行形态探测（agents 包自用副本，语义与 server core/base/config 同源）：
 * isBinaryMode 判定「TS 源码形态 vs 编译产物形态」——agents 包源码树在 dev 形态下包根有 package.json；
 * 被 bun build 内联进 server 编译产物（build-subagents 静态 import）后，产物目录无源码树结构——
 * 探测逐级向上找包根 package.json（shared/ → src/ → agents/），任一命中即 dev 形态；
 * dist/binary 形态逐级均缺失判定为二进制。resolveGebaiHome 与引擎实现完全一致。
 */
import { existsSync } from "node:fs"
import { homedir } from "node:os"
import { join } from "node:path"

/** 是否二进制（--compile）/dist 形态：自身路径逐级向上（至多三级到包根）无 package.json。 */
export function isBinaryMode(): boolean {
  let cur = import.meta.dirname
  for (let i = 0; i < 3; i++) {
    if (existsSync(join(cur, "package.json"))) return false
    cur = join(cur, "..")
  }
  return true
}

/** GEBAI_HOME 解析：显式环境变量 > 二进制形态 ~/.gebai > dev 形态仓库根（monorepo 上六级）。 */
export function resolveGebaiHome(): string {
  if (process.env.GEBAI_HOME) return process.env.GEBAI_HOME
  if (isBinaryMode()) return join(homedir(), ".gebai")
  // dev 形态：packages/agents/src/shared → 仓库根五级上
  return join(import.meta.dirname, "..", "..", "..", "..", "..")
}
