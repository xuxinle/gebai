/** 脚本输出的空白与转义符净化（与 server 侧 support/script-output 同规则；
 *  子Agent 不能跨包引用 server 内部模块，故此处自包含一份最小实现）。 */

/** ANSI 转义序列（颜色/光标控制）。 */
const ANSI_RE = /\u001b\[[0-9;?]*[A-Za-z]/g
/** 连续空行（≥2 个空行）压成一个空行。 */
const BLANK_RUN_RE = /\n(?:[ \t]*\n){2,}/g

/** 去 ANSI 控制序列、CRLF → LF、去行尾空白、连续空行压缩、去首尾空行。正文内容一律原样保留。 */
export function cleanScriptOutput(text: string): string {
  if (!text) return text
  return text
    .replace(ANSI_RE, "")
    .replace(/\r\n/g, "\n")
    .replace(/\r/g, "\n")
    .split("\n")
    .map((l) => l.replace(/[ \t]+$/, ""))
    .join("\n")
    .replace(BLANK_RUN_RE, "\n\n")
    .replace(/^\n+/, "")
    .replace(/\n+$/, "")
}
