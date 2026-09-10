/**
 * 二开依赖组件示例：custom/core/{lib}/index.ts，子代理经 `../../core/my_lib` 相对引用（见 README）。
 */
export function greet(name: string): string {
  return `hello from custom/core, ${name}`
}
