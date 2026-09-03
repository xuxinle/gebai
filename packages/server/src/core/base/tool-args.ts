/** 工具调用容错层（模型误差自适应，DESIGN「工具名与参数命名契约」）：工具名与参数键统一蛇形契约，
 *  模型偶发的分隔符/命名风格偏差（agent.run / agentRun / oldString 等）在派发点归一到规范名后解析执行，
 *  一次可恢复——不因风格误差整批调用以「未知工具/缺少必填参数」失败。引擎主循环/新会话循环与
 *  js 脚本 RPC 桥三个派发点共用；已合规的蛇形名/键恒等（无副作用）。 */
import type { Tool } from "./types"

/** 工具名容错归一：`.`/`-`/`:` 分隔符 → `_`、驼峰拆点（含连续大写缩写 HTTPFetch→http_fetch）、统一小写。 */
export function tolerantToolName(name: string): string {
  let s = name.replace(/[.\-:]+/g, "_")
  s = s.replace(/([a-z0-9])([A-Z])/g, "$1_$2").replace(/([A-Z]+)([A-Z][a-z])/g, "$1_$2")
  return s.toLowerCase()
}

/** 键指纹：去除非字母数字并小写（oldString / old_string / old-string 同指纹）。 */
function keyFingerprint(k: string): string {
  return k.replace(/[^a-zA-Z0-9]/g, "").toLowerCase()
}

function propsOf(node: unknown): Record<string, unknown> | undefined {
  if (!node || typeof node !== "object") return undefined
  const p = (node as { properties?: unknown }).properties
  return p && typeof p === "object" && !Array.isArray(p) ? (p as Record<string, unknown>) : undefined
}

/** 按参数键指纹归一单个对象（schema 驱动，递归）：精确键优先；非精确键按指纹匹配 schema 键；
 *  schema 外的键原样保留（任意透传参数不受影响）。指纹冲突（两 schema 键同指纹）的键只认精确匹配。 */
function normalizeObject(obj: Record<string, unknown>, props: Record<string, unknown>): Record<string, unknown> {
  const byFp = new Map<string, string>()
  const ambiguous = new Set<string>()
  for (const k of Object.keys(props)) {
    const f = keyFingerprint(k)
    if (byFp.has(f)) ambiguous.add(f)
    else byFp.set(f, k)
  }
  for (const f of ambiguous) byFp.delete(f)
  const out: Record<string, unknown> = {}
  for (const [k, v] of Object.entries(obj)) {
    const canon = k in props ? k : (byFp.get(keyFingerprint(k)) ?? k)
    out[canon] = normalizeValue(v, props[canon] ?? props[k])
  }
  return out
}

function normalizeValue(v: unknown, node: unknown): unknown {
  if (Array.isArray(v)) {
    const itemProps = propsOf((node as { items?: unknown } | undefined)?.items)
    if (!itemProps) return v
    return v.map((x) => (x && typeof x === "object" && !Array.isArray(x) ? normalizeObject(x as Record<string, unknown>, itemProps) : x))
  }
  if (v && typeof v === "object") {
    const p = propsOf(node)
    return p ? normalizeObject(v as Record<string, unknown>, p) : v
  }
  return v
}

/** 工具参数容错归一（schema 驱动递归）：入参键按工具 schema 的属性键做指纹匹配——驼峰/连字符/大小写
 *  偏差归一到 schema 声明的蛇形键；嵌套对象与数组元素按各自子 schema 递归；schema 未声明的键不动。 */
export function normalizeToolArgs(tool: Tool, args: Record<string, unknown>): Record<string, unknown> {
  const props = propsOf(tool.parameters)
  if (!props || !args || typeof args !== "object") return args
  return normalizeObject(args, props)
}
