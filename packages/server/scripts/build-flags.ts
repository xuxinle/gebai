/**
 * 构建期开关读取（`GEBAI_BUILD_*`）：按需跳过内嵌产物生成，供领域专用镜像裁剪。
 *
 * 关闭语义：值为 `0` / `false` / `off` / `no`（大小写不敏感、允许首尾空白）即为关闭；
 * 未设置或其它值取缺省（开启）。跳过一律**写空产物而非不生成文件**——产物被源码静态
 * import（`bun --compile` 会解析字面量路径），文件缺失会让整个构建失败；空产物由消费侧
 * 走「未内嵌」降级路径（如 D2 后端渲染不可用、grep 回退内置遍历引擎）。
 */
export function buildFlag(name: string, fallback = true): boolean {
  const v = process.env[name]?.trim().toLowerCase()
  if (!v) return fallback
  return !["0", "false", "off", "no"].includes(v)
}
