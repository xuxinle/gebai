/** md 文本模块的类型声明（bun text loader：import x from "./y.md" 返回字符串）。 */
declare module "*.md" {
  const content: string
  export default content
}
declare module "*.md?text" {
  const content: string
  export default content
}
