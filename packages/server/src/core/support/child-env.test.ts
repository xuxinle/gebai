/**
 * 脚本子进程 env 的统一口径单测：两条硬约束各自的回归。
 *
 * ① PATH 键名（Windows 上裸命令名解析成败）；② Python UTF-8 模式默认值（系统代码页的编码陷阱）。
 * 两者的错误形态都是**静默的**：少一个大写 `PATH` → 子进程仍启动、只是解析到了 WindowsApps 别名存根
 * （exit 49、stderr 为空）；少 `PYTHONUTF8=1` → 打印可能正常、`open()` 却按 cp936 走
 * （写中文/emoji 才炸，或默默落成 GBK 字节）。没有这组用例，只能靠「某台 Windows 机器上某个测试恰好红了」
 * 发现，红出来的还是一句指向错误方向的报错。
 */
import { describe, expect, test } from "bun:test"
import { withCanonicalPath, withPythonUtf8, withScriptEnvDefaults } from "./child-env"

describe("子进程 env 的 PATH 归一化", () => {
  test("Windows 规范的 Path 键 → 归一为大写 PATH（缺了它裸命令名解析会落到别名存根）", () => {
    expect(withCanonicalPath({ Path: "C:\\a;C:\\b", FOO: "1" })).toEqual({ PATH: "C:\\a;C:\\b", FOO: "1" })
  })

  test("大小写变体并存只留一个（后者胜，与展开覆盖的直觉一致）", () => {
    expect(withCanonicalPath({ Path: "old", PATH: "new" })).toEqual({ PATH: "new" })
    expect(withCanonicalPath({ PATH: "new", Path: "old" })).toEqual({ PATH: "old" })
  })

  test("已是规范大写 PATH 时原样保留（幂等：重复归一不改变结果）", () => {
    const once = withCanonicalPath({ PATH: "/usr/bin", A: "1" })
    expect(once).toEqual({ PATH: "/usr/bin", A: "1" })
    expect(withCanonicalPath(once)).toEqual(once)
  })

  test("空串 PATH 也是「有」——不能因为假值就丢掉（丢掉 = 回退父进程 PATH）", () => {
    expect(withCanonicalPath({ Path: "", FOO: "1" })).toEqual({ PATH: "", FOO: "1" })
  })

  test("没有 PATH 类键时不凭空造一个（POSIX 下继承语义可用；造空值会破坏解析）", () => {
    expect(withCanonicalPath({ FOO: "1" })).toEqual({ FOO: "1" })
    expect("PATH" in withCanonicalPath({ FOO: "1" })).toBe(false)
  })

  test("undefined 值一律丢弃（spawn 的 env 值必须是字符串）", () => {
    expect(withCanonicalPath({ A: "1", B: undefined, Path: "x" })).toEqual({ A: "1", PATH: "x" })
  })

  test("除 PATH 外的键名原样保留（含已是奇怪大小写的业务变量）", () => {
    const env = { gebai_home: "C:\\h", MixedCase: "v", Path: "p" }
    expect(withCanonicalPath(env)).toEqual({ gebai_home: "C:\\h", MixedCase: "v", PATH: "p" })
  })

  test("不修改入参（调用方还持着 process.env 的展开副本）", () => {
    const src = { Path: "x", FOO: "1" }
    withCanonicalPath(src)
    expect(src).toEqual({ Path: "x", FOO: "1" })
  })
})

describe("子进程 env 的编码默认值（Python UTF-8 模式）", () => {
  test("未设置时补 PYTHONUTF8=1（Windows 默认代码页的编码陷阱）", () => {
    expect(withPythonUtf8({ A: "1" })).toEqual({ A: "1", PYTHONUTF8: "1" })
  })

  test("用户/任务 env 显式设置则尊重（含 0——那是关闭 UTF-8 模式的唯一手段）", () => {
    expect(withPythonUtf8({ PYTHONUTF8: "0" })).toEqual({ PYTHONUTF8: "0" })
    expect(withPythonUtf8({ PYTHONUTF8: "" })).toEqual({ PYTHONUTF8: "" })
  })

  test("幂等：重复调用不再叠加键", () => {
    const once = withPythonUtf8({ A: "1" })
    expect(withPythonUtf8(once)).toEqual(once)
  })

  test("最终口径：编码默认值 + PATH 归一一次到位（PATH 归一在最后）", () => {
    expect(withScriptEnvDefaults({ Path: "C:\\a", FOO: "1" })).toEqual({ PATH: "C:\\a", FOO: "1", PYTHONUTF8: "1" })
    expect(withScriptEnvDefaults({ PYTHONUTF8: "0", PATH: "p" })).toEqual({ PYTHONUTF8: "0", PATH: "p" })
  })

  test("最终口径不修改入参", () => {
    const src = { Path: "x" }
    withScriptEnvDefaults(src)
    expect(src).toEqual({ Path: "x" })
  })
})
