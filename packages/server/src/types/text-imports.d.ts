// Bun 原生支持将 .md/.txt 等文本文件作为字符串导入（构建时随 ts 一起内联进产物）。
// 此处为 TypeScript 提供类型声明，供子 Agent 目录形式导入独立系统提示词 md 文件使用。
declare module "*.md" {
  const content: string
  export default content
}
