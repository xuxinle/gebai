/**
 * 测试进程环境净化（bunfig.toml `[test] preload` 挂载，每个测试子进程启动时先于测试文件执行）。
 *
 * 动机：测试结果不得依赖宿主环境——
 * - Bun 在 `bun test` 时按 cwd 自动加载 `.env`（仓库根 `.env` 的 GEBAI_APPROVAL_SKIP/GEBAI_SELF_MODIFY/
 *   GEBAI_LLM_* 会直接改变引擎审批、写守卫、Provider 解析等断言结果；实测「有 .env 则引擎审批类用例
 *   整批失败」）；
 * - 宿主 shell 也可能带着 GEBAI_ 与 CODE_ 前缀变量（本地调试残留）。
 *
 * 处理：启动时清除全部 `GEBAI_`/`CODE_` 前缀变量（测试用例自身在用例内显式设置并恢复的变量不受影响——
 * preload 只作用于进程启动瞬间）。核心模块另有 test 期防线：`loadConfig` 的 `loadDotEnv` 在
 * `NODE_ENV === "test"` 时跳过读取仓库 `.env`（否则用例内首次 loadConfig 会把 .env 重新注入 process.env）。
 */
for (const key of Object.keys(process.env)) {
  if (key.startsWith("GEBAI_") || key.startsWith("CODE_")) delete process.env[key]
}
