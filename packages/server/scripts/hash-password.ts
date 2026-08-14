/**
 * 生成服务模式 admin 密码哈希（GEBAI_ADMIN_PASSWORD_HASH 的值）。
 *
 * 用法（三种等价方式）：
 *   bun run --cwd packages/server hash-password -- '密码'     # 直接传参（警示：密码会留在 shell history）
 *   echo -n '密码' | bun run --cwd packages/server hash-password   # 管道传入（推荐）
 *   bun run --cwd packages/server hash-password              # 交互输入（从 stdin 读取）
 *
 * 输出格式：`salt:hash`（均为 hex，salt 16 字节 / hash 64 字节，与注册表 scrypt 加盐哈希完全一致）。
 * 将输出值写入服务模式启动环境变量 GEBAI_ADMIN_PASSWORD_HASH 即可启用 admin 用户。
 */
import { randomBytes, scryptSync } from "node:crypto"

async function readStdin(): Promise<string> {
  let buf = ""
  for await (const chunk of process.stdin) buf += String(chunk)
  return buf.trim()
}

let password = process.argv[2] ?? ""
if (!password) {
  console.error("[gebai] 请输入密码（stdin，回车确认）：")
  password = await readStdin()
}
if (!password) {
  console.error("[gebai] 错误：密码为空")
  process.exit(1)
}
const salt = randomBytes(16).toString("hex")
const hash = scryptSync(password, salt, 64).toString("hex")
console.log(`${salt}:${hash}`)
