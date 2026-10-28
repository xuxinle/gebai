/**
 * 运行时数据根（`{GEBAI_HOME}/data/`）的单测。
 *
 * 重点验三件事——每一条都对应一种真实会踩的故障：
 * 1. 路径公式本身（`data/` 那层不能丢，dev 形态下它就是"代码目录变干净"的全部来源）；
 * 2. **旧位置回退读**（升级后不能"记忆/知识库全空"）；
 * 3. **幂等迁移**（搬过的不能再搬、目标已有的不能覆盖——覆盖会丢用户新数据）。
 */
import { describe, expect, test } from "bun:test"
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { DATA_DIRNAME, dataDir, dataSubdir, migrateLegacyDir, resolveDataPath } from "./data-dir"

const tmp = (): string => mkdtempSync(join(tmpdir(), "gebai-data-"))
const cleanup = (...dirs: string[]) => {
  for (const d of dirs) rmSync(d, { recursive: true, force: true })
}

describe("路径公式", () => {
  test("dataDir 在 home 下加一层 data", () => {
    expect(dataDir("/home/u")).toBe(join("/home/u", DATA_DIRNAME))
    expect(DATA_DIRNAME).toBe("data")
  })

  test("dataSubdir 拼到 data 下的子目录（persona/xiangqi/knowledge 三个用户点名的都走它）", () => {
    expect(dataSubdir("/h", "persona")).toBe(join("/h", "data", "persona"))
    expect(dataSubdir("/h", "xiangqi")).toBe(join("/h", "data", "xiangqi"))
    expect(dataSubdir("/h", "knowledge")).toBe(join("/h", "data", "knowledge"))
  })

  test("home 为空串时回退 cwd（临时实例/测试的「就地自足」口径）", () => {
    expect(dataDir("")).toBe(join(process.cwd(), DATA_DIRNAME))
    // 纯空白也按空处理：环境变量写成空格不该算出 `" /data"` 这种相对路径
    expect(dataDir("   ")).toBe(join(process.cwd(), DATA_DIRNAME))
  })
})

describe("读路径：新位置优先、旧位置回退", () => {
  test("新位置存在 → 用新位置（即便旧位置也有）", () => {
    const home = tmp()
    try {
      mkdirSync(join(home, "data", "persona"), { recursive: true })
      mkdirSync(join(home, "persona"), { recursive: true })
      expect(resolveDataPath(home, "persona", "persona")).toBe(join(home, "data", "persona"))
    } finally {
      cleanup(home)
    }
  })

  test("新位置不存在、旧位置存在 → 回退旧位置（升级后照常读到原有数据）", () => {
    const home = tmp()
    try {
      mkdirSync(join(home, "knowledge"), { recursive: true })
      writeFileSync(join(home, "knowledge", "cefu.json"), "{}")
      expect(resolveDataPath(home, "knowledge", "knowledge")).toBe(join(home, "knowledge"))
    } finally {
      cleanup(home)
    }
  })

  test("两处都没有 → 返回新位置（让写入方在正确的地方创建）", () => {
    const home = tmp()
    try {
      expect(resolveDataPath(home, "persona", "persona")).toBe(join(home, "data", "persona"))
    } finally {
      cleanup(home)
    }
  })

  test("没有旧位置（新引入的数据）→ 行为等同 dataSubdir", () => {
    const home = tmp()
    try {
      expect(resolveDataPath(home, "brand-new")).toBe(join(home, "data", "brand-new"))
    } finally {
      cleanup(home)
    }
  })
})

describe("迁移：幂等、不覆盖、失败不抛", () => {
  test("旧目录内容搬进 data/，文件内容原样", () => {
    const home = tmp()
    try {
      mkdirSync(join(home, "persona", "outbox"), { recursive: true })
      writeFileSync(join(home, "persona", "memory.md"), "## 关于用户\n喜欢简洁\n")
      writeFileSync(join(home, "persona", "outbox", "a.txt"), "产物")

      const r = migrateLegacyDir(home, "persona")
      expect(r.error).toBeNull()
      expect(r.moved).toBe(2)
      expect(r.skipped).toBe(0)
      // 内容真的到位了（不是只建了空目录）
      expect(readFileSync(join(home, "data", "persona", "memory.md"), "utf8")).toContain("喜欢简洁")
      expect(readFileSync(join(home, "data", "persona", "outbox", "a.txt"), "utf8")).toBe("产物")
    } finally {
      cleanup(home)
    }
  })

  test("幂等：连搬两次，第二次搬 0 个（已搬过的不重复搬）", () => {
    const home = tmp()
    try {
      mkdirSync(join(home, "persona"), { recursive: true })
      writeFileSync(join(home, "persona", "memory.md"), "x")
      expect(migrateLegacyDir(home, "persona").moved).toBe(1)
      const second = migrateLegacyDir(home, "persona")
      expect(second.moved).toBe(0)
      // 源已被搬走（rename 掉了），所以也不算"跳过"
      expect(second.error).toBeNull()
    } finally {
      cleanup(home)
    }
  })

  test("不覆盖：目标已有同名文件时保留目标内容（新位置数据永远优先）", () => {
    const home = tmp()
    try {
      mkdirSync(join(home, "data", "persona"), { recursive: true })
      writeFileSync(join(home, "data", "persona", "memory.md"), "新位置的较新内容")
      mkdirSync(join(home, "persona"), { recursive: true })
      writeFileSync(join(home, "persona", "memory.md"), "旧位置的旧内容")

      const r = migrateLegacyDir(home, "persona")
      expect(r.skipped).toBe(1)
      expect(r.moved).toBe(0)
      expect(readFileSync(join(home, "data", "persona", "memory.md"), "utf8")).toBe("新位置的较新内容")
    } finally {
      cleanup(home)
    }
  })

  test("旧目录不存在 → 空操作，不报错（绝大多数新部署的常态）", () => {
    const home = tmp()
    try {
      const r = migrateLegacyDir(home, "persona")
      expect(r.moved).toBe(0)
      expect(r.error).toBeNull()
      // 不该因为"没东西搬"就凭空建出 data/ 下的空目录树
      expect(existsSync(join(home, "data", "persona"))).toBe(false)
    } finally {
      cleanup(home)
    }
  })

  test("数据名可与旧目录名不同（供将来把 channel-drop 之类改名的场合）", () => {
    const home = tmp()
    try {
      mkdirSync(join(home, "old-name"), { recursive: true })
      writeFileSync(join(home, "old-name", "f.txt"), "v")
      const r = migrateLegacyDir(home, "old-name", "new-name")
      expect(r.moved).toBe(1)
      expect(existsSync(join(home, "data", "new-name", "f.txt"))).toBe(true)
    } finally {
      cleanup(home)
    }
  })
})
