/** 脚本类工具输出（sh/py/js/后台任务日志）的纯净化：去掉不影响语义的噪声，模型读到的字节更少。 */

/** ANSI 转义序列（颜色/光标控制）：子进程输出带终端控制符时模型看到的是乱码字符与无效 token。 */
const ANSI_RE = /\u001b\[[0-9;?]*[A-Za-z]/g
/** 连续空行（≥2 个空行）压成一个空行：保留段落分隔，去掉堆积的垂直空白。 */
const BLANK_RUN_RE = /\n(?:[ \t]*\n){2,}/g

/**
 * 清洗脚本输出（不改变可读内容与顺序）：
 * - 去 ANSI 控制序列（Windows/彩色输出常见）
 * - CRLF → LF（Windows 子进程输出成对 \r\n，\r 对模型无意义）
 * - 去行尾空白
 * - 连续空行压成一个空行
 * - 去首尾空白行
 *
 * 行号、路径、错误文本等正文一律原样保留——本函数只动空白与转义符。
 */
export function cleanScriptOutput(text: string): string {
  if (!text) return text
  const noAnsi = text.replace(ANSI_RE, "")
  const lf = noAnsi.replace(/\r\n/g, "\n").replace(/\r/g, "\n")
  const trimmedLines = lf
    .split("\n")
    .map((l) => l.replace(/[ \t]+$/, ""))
    .join("\n")
  const compact = trimmedLines.replace(BLANK_RUN_RE, "\n\n")
  return compact.replace(/^\n+/, "").replace(/\n+$/, "")
}
