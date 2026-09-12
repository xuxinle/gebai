import { afterEach, beforeEach, describe, expect, test } from "bun:test"
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { JSON_LOCK_GRACE_MS, JSON_LOCK_LEASE_MS, mutateJsonList, readJsonList, withFileLock, writeJsonListAtomic } from "./json-store"

let home = ""
const fileOf = (name = "todos.json") => join(home, name)

interface Item {
  id: string
  text: string
}
const normalize = (raw: unknown): Item | null => {
  if (!raw || typeof raw !== "object") return null
  const e = raw as Item
  if (typeof e.id !== "string" || typeof e.text !== "string") return null
  return { id: e.id, text: e.text }
}
const read = (f: string): Item[] => JSON.parse(readFileSync(f, "utf8")) as Item[]

beforeEach(() => {
  home = mkdtempSync(join(tmpdir(), "gebai-jsonstore-"))
})
afterEach(() => rmSync(home, { recursive: true, force: true }))

describe("跨进程写锁", () => {
  test("互斥：并发两次进入临界区不重叠（串行执行）", async () => {
    const order: string[] = []
    const lock = join(home, "x.lock")
    const task = (name: string, ms: number) =>
      withFileLock(lock, async () => {
        order.push(`${name}:enter`)
        await new Promise((r) => setTimeout(r, ms))
        order.push(`${name}:exit`)
      })
    await Promise.all([task("a", 40), task("b", 10)])
    // 任一时刻只有一个在临界区：enter/exit 必须成对相邻
    expect(order).toHaveLength(4)
    expect(order[0]!.endsWith(":enter")).toBe(true)
    expect(order[1]!).toBe(`${order[0]!.split(":")[0]}:exit`)
  })

  test("陈旧锁（持有者 PID 已死）被抢占，不阻塞", async () => {
    const lock = join(home, "stale.lock")
    writeFileSync(lock, JSON.stringify({ pid: 999_999, at: Date.now() }))
    const got = await withFileLock(lock, async () => "ok", { timeoutMs: 1000 })
    expect(got).toBe("ok")
  })

  test("租约过期（持有者 PID 存活但久未释放）被抢占", async () => {
    const lock = join(home, "expired.lock")
    // 持有者写成本进程 PID（存活），但 at 远早于租约 → 视为已死
    writeFileSync(lock, JSON.stringify({ pid: process.pid, at: Date.now() - JSON_LOCK_LEASE_MS - 1000 }))
    const got = await withFileLock(lock, async () => "ok", { timeoutMs: 1000 })
    expect(got).toBe("ok")
  })

  test("空白/损坏锁在宽限期内视为「正在持有」，不误抢占（O_EXCL 创建与写内容的窗口）", async () => {
    const lock = join(home, "blank.lock")
    writeFileSync(lock, "") // 刚创建、内容尚未写入（mtime 即当前）
    await expect(withFileLock(lock, async () => "never", { timeoutMs: 150 })).rejects.toThrow(/获取写锁超时/)
  })

  test("空白锁超出宽限期按陈旧抢占（崩溃残留不永久阻塞写入）", async () => {
    const lock = join(home, "stale-blank.lock")
    writeFileSync(lock, "{ not json")
    // 用可注入时钟把「现在」推到宽限期之后（不改 mtime，避免依赖文件系统时间精度）
    const got = await withFileLock(lock, async () => 1, { timeoutMs: 1000, now: () => Date.now() + JSON_LOCK_GRACE_MS + 1000 })
    expect(got).toBe(1)
  })

  test("活跃持有者占锁超时抛错（不降级为无锁写）", async () => {
    const lock = join(home, "busy.lock")
    await withFileLock(lock, async () => {
      await expect(withFileLock(lock, async () => "never", { timeoutMs: 120 })).rejects.toThrow(/获取写锁超时/)
    })
  })

  test("fn 抛错时锁仍被释放", async () => {
    const lock = join(home, "err.lock")
    await expect(withFileLock(lock, async () => { throw new Error("boom") })).rejects.toThrow("boom")
    expect(existsSync(lock)).toBe(false)
    expect(await withFileLock(lock, async () => "ok", { timeoutMs: 500 })).toBe("ok")
  })
})

describe("宽容读取与原子写", () => {
  test("缺失/损坏/非数组 → 空清单；条目经 normalize 过滤", async () => {
    expect(await readJsonList(fileOf("none.json"), normalize)).toEqual([])
    writeFileSync(fileOf("bad.json"), "{ broken")
    expect(await readJsonList(fileOf("bad.json"), normalize)).toEqual([])
    writeFileSync(fileOf("obj.json"), JSON.stringify({ a: 1 }))
    expect(await readJsonList(fileOf("obj.json"), normalize)).toEqual([])
    writeFileSync(fileOf("mixed.json"), JSON.stringify([{ id: "a", text: "x" }, { id: 5 }, null, "s"]))
    expect(await readJsonList(fileOf("mixed.json"), normalize)).toEqual([{ id: "a", text: "x" }])
  })

  test("原子写：父目录自动创建，无 .tmp 残留", async () => {
    const f = join(home, "deep", "dir", "todos.json")
    await writeJsonListAtomic(f, [{ id: "a", text: "x" }])
    expect(read(f)).toEqual([{ id: "a", text: "x" }])
    expect(existsSync(`${f}.${process.pid}.tmp`)).toBe(false)
  })

  test("滚动备份：覆盖前留存现值；原内容为空则不备份", async () => {
    const f = fileOf("backup.json")
    await writeJsonListAtomic(f, [{ id: "a", text: "one" }])
    expect(existsSync(`${f}.bak`)).toBe(false) // 首次写无现值可备份
    await writeJsonListAtomic(f, [{ id: "a", text: "two" }])
    expect(JSON.parse(readFileSync(`${f}.bak`, "utf8"))).toEqual([{ id: "a", text: "one" }])
    // 空清单不覆盖上一份可用备份
    writeFileSync(f, "[]")
    await writeJsonListAtomic(f, [{ id: "a", text: "three" }])
    expect(JSON.parse(readFileSync(`${f}.bak`, "utf8"))).toEqual([{ id: "a", text: "one" }])
  })
})

describe("RMW：以磁盘真值为合并基准（数据丢失修复的核心）", () => {
  test("陈旧镜像不再具有破坏性：变更函数看到的是磁盘内容", async () => {
    const f = fileOf()
    writeFileSync(f, JSON.stringify([{ id: "a", text: "A" }, { id: "b", text: "B" }]))
    let seen: Item[] = []
    await mutateJsonList(
      f,
      (disk) => {
        seen = disk // 记录变更函数实际看到的基准
        return [...disk, { id: "c", text: "C" }]
      },
      { normalize },
    )
    expect(seen.map((e) => e.id)).toEqual(["a", "b"]) // 磁盘真值，而非空/陈旧镜像
    expect(read(f).map((e) => e.id)).toEqual(["a", "b", "c"])
  })

  test("删除一条不误删其他进程新增的条目（旧实现会整体覆盖丢条目）", async () => {
    const f = fileOf()
    // 磁盘上有三条：a、b 为本进程所知，c 是另一实例刚写入的
    writeFileSync(f, JSON.stringify([{ id: "a", text: "A" }, { id: "b", text: "B" }, { id: "c", text: "C" }]))
    const after = await mutateJsonList(f, (disk) => disk.filter((e) => e.id !== "a"), { normalize })
    expect(after.map((e) => e.id)).toEqual(["b", "c"]) // c 保留（合并基准是磁盘真值）
    expect(read(f).map((e) => e.id)).toEqual(["b", "c"])
  })

  test("返回落盘后的真值（调用方据此同步本地镜像）", async () => {
    const f = fileOf()
    writeFileSync(f, JSON.stringify([{ id: "a", text: "A" }]))
    const out = await mutateJsonList(f, (disk) => disk.map((e) => ({ ...e, text: "A2" })), { normalize })
    expect(out).toEqual([{ id: "a", text: "A2" }])
  })

  test("无实质变更不写盘（不抖动 mtime、不产生备份）", async () => {
    const f = fileOf()
    writeFileSync(f, JSON.stringify([{ id: "a", text: "A" }]))
    const before = readFileSync(f, "utf8")
    await mutateJsonList(f, (disk) => disk.map((e) => ({ ...e })), { normalize }) // 等值复制
    expect(readFileSync(f, "utf8")).toBe(before)
    expect(existsSync(`${f}.bak`)).toBe(false)
  })

  test("文件不存在且结果为空：不写入空文件", async () => {
    const f = fileOf("absent.json")
    const out = await mutateJsonList(f, () => [], { normalize })
    expect(out).toEqual([])
    expect(existsSync(f)).toBe(false)
  })

  test("写前复核发现非协作写者改动 → 重读重试，不覆盖其新内容", async () => {
    const f = fileOf()
    writeFileSync(f, JSON.stringify([{ id: "a", text: "A" }]))
    let calls = 0
    const out = await mutateJsonList(
      f,
      (disk) => {
        calls++
        // 首次进入时模拟「外部进程/旧版本进程」直接改写文件（不走锁）
        if (calls === 1) writeFileSync(f, JSON.stringify([...disk, { id: "ext", text: "EXTERNAL" }]))
        return [...disk, { id: `n${calls}`, text: "N" }]
      },
      { normalize },
    )
    expect(calls).toBe(2) // 第一次被 mtime 复核拦下并重试
    expect(out.map((e) => e.id)).toEqual(["a", "ext", "n2"]) // 外部写入被保留
    expect(read(f).map((e) => e.id)).toEqual(["a", "ext", "n2"])
  })

  test("持续冲突达重试上限后抛错（不静默丢数据）", async () => {
    const f = fileOf()
    writeFileSync(f, JSON.stringify([{ id: "a", text: "A" }]))
    let n = 0
    await expect(
      mutateJsonList(
        f,
        (disk) => {
          writeFileSync(f, JSON.stringify([...disk, { id: `x${++n}`, text: "X" }])) // 每次都在复核窗口内改动
          return [...disk, { id: "mine", text: "M" }]
        },
        { normalize, retries: 2 },
      ),
    ).rejects.toThrow(/并发写入冲突/)
  })

  test("锁在变更函数抛错时释放，后续写入可正常进行", async () => {
    const f = fileOf()
    await expect(
      mutateJsonList(f, () => { throw new Error("mutate failed") }, { normalize }),
    ).rejects.toThrow("mutate failed")
    expect(existsSync(`${f}.lock`)).toBe(false)
    const out = await mutateJsonList(f, () => [{ id: "ok", text: "OK" }], { normalize })
    expect(out).toEqual([{ id: "ok", text: "OK" }])
  })
})
