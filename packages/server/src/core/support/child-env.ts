/**
 * 子进程环境的 PATH 归一化（**Windows 裸命令名解析的硬约束**）。
 *
 * ## 为什么必须做这件事（一次实测出来的静默失效）
 *
 * Windows 上 `process.env` 的 PATH 键名是 **`Path`**（首字母大写、其余小写），这是系统的规范写法。
 * 但 Node / Bun 的 `spawn(file, ...)` 在 `file` 不含路径、需要**按名解析可执行文件**时，
 * 只会去传入的 env 对象里找**精确大写 `PATH`** 这一个键：
 *
 *   - 有 `PATH` → 用它（按值里的目录顺序解析，含我们自己前置的目录）；
 *   - 只有 `Path` → 视同「没有 PATH」，回退到**父进程的 PATH**（CreateProcess 的既有语义）。
 *
 * 后者在本机不是「找不到」而是「找到错的那个」：`%LOCALAPPDATA%\Microsoft\WindowsApps\python.exe`
 * 是微软商店的**应用执行别名存根**（0 字节重解析点），它排在真 Python 之前，被执行时直接返回
 * **9009**（命令找不到），落到 8 位退出码就是 **49**、stderr 为空。用户/我们看到的只有一行
 * `[exit 49]`，没有任何线索指向「解析到了别名存根」。
 *
 * 实测（Windows 11 + Bun 1.3.14，同一台机、同一个裸命令名 `python`）：
 *
 *   | 传给子进程的 env        | 解析结果                        |
 *   |------------------------|---------------------------------|
 *   | `{ PATH: <真值> }`      | 真 Python 3.12（WindowsApps 前置时也按序解析，且 Bun 会跳过别名存根） |
 *   | `{ Path: <真值> }`      | WindowsApps 存根 → exit 49      |
 *   | 不传 env（继承）         | 真 Python 3.12                  |
 *
 * 另一条实测（同样重要，且更容易误判）：**在 Windows 的 PowerShell 里直接敲 `python` 也会是 49**——
 * 那是 shell 自己的 PATH 搜索命中了存根，与本模块无关，但也正因如此，「PowerShell 里 `python` 不通」
 * 不能当作「本机没有 Python」的证据；`py`（官方启动器）通常才是能用的那个。
 *
 * 所以凡是「自己拼一份 env 再 spawn 裸命令名」的地方（sh/py/js 脚本子进程、任务级 env、
 * 会话 scriptEnv 合并后的环境），都必须保证**落地时只有一个、且键名为大写 `PATH`**。
 *
 * ## 口径
 *
 * 1. **大小写变体只保留一个**：env 块里同时存在 `Path` 与 `PATH` 时，不同运行时/不同子进程工具的
 *    读取顺序不一致（Windows 环境块本身允许同名不同大小写并存），那是「同一台机上行为看运气」。
 *    故一律去重，只留大写 `PATH`（Windows 环境变量名本就不区分大小写，PowerShell 的 `$env:Path`
 *    与 cmd 的 `%PATH%` 都照常命中）。
 * 2. **值为空串也算「有」**：显式传 `PATH: ""` 的语义是「不要任何搜索路径」，与「没这个键」不是一回事
 *    （后者会回退父进程 PATH）——这一条必须原样保留，否则调用方想收紧搜索路径的努力会被静默吞掉。
 * 3. **`undefined` 的值一律丢弃**：Bun/Node 对 env 值的要求是字符串；`{...process.env}` 在类型上
 *    可能带 `undefined`（见 `scriptChildEnv` 的既有处理），传下去是运行时错误。
 */

/** 是否 PATH 键（Windows 环境变量名不区分大小写）。 */
function isPathKey(k: string): boolean {
  return k.toLowerCase() === "path"
}

/**
 * 归一出「可安全传给 spawn 的环境」：PATH 类键去重为大写 `PATH`，丢弃 `undefined` 值，其余原样保留。
 *
 * 不做任何脱敏/收敛判断（那是 `scriptChildEnv` 与 `Sandbox.exec` 各自的事）：本函数只管
 * 「env 块本身的键名规范」，这样各处拼出来的环境都能直接喂给 `spawn`。纯函数，可单测。
 */
export function withCanonicalPath(env: Record<string, string | undefined>): Record<string, string> {
  const out: Record<string, string> = {}
  let pathValue: string | undefined
  for (const [k, v] of Object.entries(env)) {
    if (v === undefined) continue
    if (isPathKey(k)) {
      // 多份大小写变体并存时**后者胜**：JS 展开惯用法里「写在后面的就是覆盖」
      // （`{...process.env, PATH: x}` 与 `{PATH: x, ...process.env}` 都读作「最后那个生效」），
      // 大小写变体视为同一个键后沿用同一套直觉。
      pathValue = v
      continue
    }
    out[k] = v
  }
  if (pathValue !== undefined) out.PATH = pathValue
  return out
}
