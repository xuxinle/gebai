/**
 * `withCanonicalPath` 单测：Windows 上裸命令名解析成败的那一条硬约束（见实现文件头的实测表）。
 *
 * 为何值得单独钉住：这个函数的错误形态是**静默的**——env 里少一个大写 `PATH`，子进程仍然启动、
 * 只是解析到了 WindowsApps 的别名存根（exit 49、stderr 为空）。没有这组用例，只能靠
 * 「某台 Windows 机器上的某个测试恰好红了」来发现，且红出来的是一句 `[exit 49]`，
 * 指向的是「python 坏了」而不是「env 键名错了」。
 */
import { describe, expect, test } from "bun:test"
import { withCanonicalPath } from "./child-env"

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
