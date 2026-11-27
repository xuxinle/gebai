/**
 * 多人会话（群）：群档案 / 成员身份 / 会话目录 / 访问隔离。
 *
 * 覆盖四件容易静默走偏的事：
 * ① 群会话**不落在任何用户目录**（`groups/{groupId}/`），且重启（重新 init）后仍能定位；
 * ② 访问权只看群成员资格——非成员 load 一律为空（含「不是群主但知道会话 id」这条最常见的越权路径）；
 * ③ 名称与角色是群内身份：同一账号在不同群可以不同，也不改账号本身；
 * ④ 进入口令 scrypt 落盘、明文不出内存；错误口令与无效邀请码分别给出不同原因。
 */
import { afterEach, beforeEach, describe, expect, it } from "bun:test"
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { GroupStore, normalizeJoinCode, normalizeMember } from "./groups"
import { SessionStore } from "./store"
import { groupDir, groupSessionDirOf, sessionPath } from "../base/paths"

const HOME = () => mkdtempSync(join(tmpdir(), "gebai-groups-"))

/** 装配 store + groups（与 boot/compose 同构：groupAccess 经 groups 判定成员资格）。 */
async function wire(home: string) {
  const groups = new GroupStore(home)
  const store = new SessionStore({
    home,
    groupAccess: (sessionId, userId) => {
      const g = groups.groupOfSession(sessionId)
      const m = g?.members.find((x) => x.userId === userId)
      return g && m ? { groupId: g.id, groupName: g.name, memberName: m.name, role: m.role } : null
    },
  })
  await groups.init()
  groups.attach({
    createGroupSession: (gid, owner, name) => store.createGroupSession(gid, owner, name),
    rename: (sid, name, uid) => store.rename(sid, name, uid),
    register: (sid, group) => store.registerGroupSession(sid, group),
    unregister: (sid) => store.unregisterGroupSession(sid),
    evict: (sid) => store.evict(sid),
  })
  return { groups, store }
}
let home = ""
beforeEach(() => {
  home = HOME()
})
afterEach(() => {
  rmSync(home, { recursive: true, force: true })
})

describe("群会话目录体系（不属于任何用户）", () => {
  it("群会话正文落在 groups/{groupId}/，不在 users/ 下", async () => {
    const { groups, store } = await wire(home)
    const g = await groups.create("alice", "alice", { name: "产品评审", memberName: "张三", memberRole: "主持人" })
    const dir = groupDir(home, g.id)
    expect(g.sessionId).toMatch(/^[0-9a-f]{32}$/)
    expect(dir).toBe(join(home, "groups", g.id))
    expect(existsSync(join(dir, "chat.json"))).toBe(true)
    expect(existsSync(join(dir, "tmp"))).toBe(true)
    expect(existsSync(join(dir, "group.json"))).toBe(true)
    // 用户目录下没有这个会话（不因为创建者是 alice 就塞进 users/alice）
    expect(existsSync(join(home, "users", "alice", "sessions"))).toBe(false)
    // sessionPath 对群会话返回群目录（引擎/沙箱/文件工作台共用这一处定位）
    expect(sessionPath(home, "alice", g.sessionId)).toBe(dir)
    // 会话正文带群归属（访问权判定的单一来源）
    const s = await store.load(g.sessionId, "alice")
    expect(s?.group?.groupId).toBe(g.id)
  })

  it("重启（重新 init）后仍能定位群会话（映射从 group.json 重建）", async () => {
    const first = await wire(home)
    const g = await first.groups.create("alice", "alice", { name: "运维值班", memberName: "A" })
    await first.store.appendMessage(g.sessionId, { id: "m1", role: "user", content: "hello", createdAt: Date.now() }, "alice")
    // 模拟进程重启：同一 home 新装配一套
    const second = await wire(home)
    const s = await second.store.load(g.sessionId, "alice")
    expect(s?.messages.map((m) => m.content)).toEqual(["hello"])
    expect(second.store.getSessionDir(g.sessionId, "alice")).toBe(groupDir(home, g.id))
    // 解散后无法再访问（映射与内存索引一并注销）
    await second.groups.dissolve("alice", g.id)
    expect(second.store.groupOfSession(g.sessionId)).toBeNull()
    expect(groupSessionDirOf(home, g.sessionId)).toBeNull()
  })
})

describe("成员资格是唯一访问判据", () => {
  it("非成员看不到群会话；成员看得到（含非群主成员）", async () => {
    const { groups, store } = await wire(home)
    const g = await groups.create("alice", "alice", { name: "需求池", memberName: "甲", memberRole: "主持人" })
    await groups.join("bob", "bob", { joinCode: g.joinCode, memberName: "乙", memberRole: "工程师" })
    expect((await store.load(g.sessionId, "bob"))?.id).toBe(g.sessionId)
    // carol 知道会话 id 也进不去（不是成员）
    expect(await store.load(g.sessionId, "carol")).toBeNull()
    // 无用户上下文的内部调用同样拒绝（不因“无人”而放行）
    expect(await store.load(g.sessionId)).toBeNull()
    // 会话列表：成员各自看得到，非成员看不到；且列表项带各自的群内身份
    const aliceList = await store.listSessionInfos("alice")
    const bobList = await store.listSessionInfos("bob")
    const carolList = await store.listSessionInfos("carol")
    expect(aliceList.find((s) => s.id === g.sessionId)?.group).toMatchObject({ id: g.id, name: "需求池", memberName: "甲", role: "主持人" })
    expect(bobList.find((s) => s.id === g.sessionId)?.group).toMatchObject({ memberName: "乙", role: "工程师" })
    expect(carolList.find((s) => s.id === g.sessionId)).toBeUndefined()
  })

  it("加入的群在会话列表里可见（跨进程重启后也可见）", async () => {
    // 回归点：会话列表只列「会话存储记得的群」——群映射只登记到文件系统层时，
    // 加入的群（新进程从未 load 过其正文）会在列表里静默消失
    const { groups } = await wire(home)
    const g = await groups.create("alice", "alice", { name: "跨用户群", memberName: "甲" })
    await groups.join("bob", "bob", { joinCode: g.joinCode, memberName: "乙" })
    // 重启：新装配一套（bob 从未加载过该会话正文）
    const second = await wire(home)
    const bobList = await second.store.listSessionInfos("bob")
    expect(bobList.find((s) => s.id === g.sessionId)).toBeDefined()
    expect((await second.store.load(g.sessionId, "bob"))?.id).toBe(g.sessionId)
  })

  it("群会话不能像单人会话那样删除（要清上下文请截断/压缩，要结束请解散）", async () => {
    const { groups, store } = await wire(home)
    const g = await groups.create("alice", "alice", { name: "临时群", memberName: "甲" })
    await expect(store.delete(g.sessionId, "alice")).rejects.toThrow(/多人会话不能单独删除/)
    expect(existsSync(join(groupDir(home, g.id), "chat.json"))).toBe(true)
  })

  it("事件受众 = 全部成员（每个成员各一份，便于各自断线重放）", async () => {
    const { groups } = await wire(home)
    const g = await groups.create("alice", "alice", { name: "站会", memberName: "甲" })
    await groups.join("bob", "bob", { joinCode: g.joinCode, memberName: "乙" })
    expect(groups.audienceOf(g.sessionId)?.sort()).toEqual(["alice", "bob"])
    expect(groups.audienceOf("0".repeat(32))).toBeNull() // 非群会话：走会话归属单播
  })
})

describe("群内身份（名称/角色不与账号绑定）", () => {
  it("同一账号在不同群可以有不同称呼；改身份不改账号", async () => {
    const { groups } = await wire(home)
    const a = await groups.create("alice", "alice", { name: "设计群", memberName: "Alice·设计", memberRole: "设计师" })
    const b = await groups.join("alice", "alice", { joinCode: a.joinCode, memberName: "x", memberRole: "y" }) // 同群改写
    expect(b.members.find((m) => m.userId === "alice")?.name).toBe("x")
    const c = await groups.create("alice", "alice", { name: "研发群", memberName: "Alice·研发", memberRole: "工程师" })
    expect(groups.speakerOf(a.sessionId, "alice")).toEqual({ name: "x", role: "y" })
    expect(groups.speakerOf(c.sessionId, "alice")).toEqual({ name: "Alice·研发", role: "工程师" })
    // 群内身份落在 group.json（不是账号注册表）
    const raw = JSON.parse(readFileSync(join(groupDir(home, a.id), "group.json"), "utf8")) as { members: Array<{ userId: string; name: string; role: string }> }
    expect(raw.members.find((m) => m.userId === "alice")).toMatchObject({ name: "x", role: "y" })
  })

  it("身份清洗：名称为空拒绝，角色缺省为「成员」", () => {
    expect(() => normalizeMember("   ", "主持人")).toThrow(/名称/)
    expect(normalizeMember(" 张三 ", "")).toEqual({ name: "张三", role: "成员" })
    expect(normalizeMember("a".repeat(80), "r")).toEqual({ name: "a".repeat(32), role: "r" })
  })

  it("进入群会按提交身份改写（再进一次 = 换称呼，不必先退出）", async () => {
    const { groups } = await wire(home)
    const g = await groups.create("alice", "alice", { name: "评审群", memberName: "甲", memberRole: "主持人" })
    await groups.join("bob", "bob", { joinCode: g.joinCode, memberName: "乙" })
    const again = await groups.join("bob", "bob", { joinCode: g.joinCode, memberName: "乙方", memberRole: "评审人" })
    expect(again.members.filter((m) => m.userId === "bob")).toHaveLength(1)
    expect(again.members.find((m) => m.userId === "bob")).toMatchObject({ name: "乙方", role: "评审人" })
  })
})

describe("邀请码与进入口令", () => {
  it("邀请码 8 位、无歧义字母表，规范化为大写（手抄带空格/小写也能进）", async () => {
    const { groups } = await wire(home)
    const g = await groups.create("alice", "alice", { name: "群", memberName: "甲" })
    expect(g.joinCode).toMatch(/^[2-9A-HJ-NP-Z]{8}$/)
    expect(normalizeJoinCode(` ${g.joinCode.toLowerCase()} `)).toBe(g.joinCode)
    const joined = await groups.join("bob", "bob", { joinCode: g.joinCode.toLowerCase(), memberName: "乙" })
    expect(joined.members.map((m) => m.userId)).toContain("bob")
  })

  it("有口令的群：不填口令/口令错误都拒绝，正确口令放行；口令只以哈希落盘", async () => {
    const { groups } = await wire(home)
    const g = await groups.create("alice", "alice", { name: "机密群", memberName: "甲", passcode: "s3cret" })
    expect(g.passcode?.hash).toBeTruthy()
    const raw = readFileSync(join(groupDir(home, g.id), "group.json"), "utf8")
    expect(raw).not.toContain("s3cret") // 明文不落盘
    await expect(groups.join("bob", "bob", { joinCode: g.joinCode, memberName: "乙" })).rejects.toThrow(/口令/)
    await expect(groups.join("bob", "bob", { joinCode: g.joinCode, passcode: "wrong", memberName: "乙" })).rejects.toThrow(/口令不正确/)
    const ok = await groups.join("bob", "bob", { joinCode: g.joinCode, passcode: "s3cret", memberName: "乙" })
    expect(ok.members.map((m) => m.userId)).toContain("bob")
    // 对外视图只暴露“有没有设口令”
    expect(groups.view(g.id, "alice")?.hasPasscode).toBe(true)
    expect(JSON.stringify(groups.view(g.id, "alice"))).not.toContain("s3cret")
    // 群主可清除口令
    const cleared = await groups.setPasscode("alice", g.id, "")
    expect(cleared.passcode).toBeUndefined()
  })

  it("无效邀请码与非成员加入都是明确失败", async () => {
    const { groups } = await wire(home)
    await expect(groups.join("bob", "bob", { joinCode: "ZZZZZZZZ", memberName: "乙" })).rejects.toThrow(/邀请码无效/)
  })
})

describe("群主操作与退出/解散", () => {
  it("只有群主能改名/设口令/解散；群主不能直接退群（先转让或解散）", async () => {
    const { groups, store } = await wire(home)
    const g = await groups.create("alice", "alice", { name: "旧名", memberName: "甲" })
    await groups.join("bob", "bob", { joinCode: g.joinCode, memberName: "乙" })
    await expect(groups.rename("bob", g.id, "盗名")).rejects.toThrow(/群主/)
    await expect(groups.setPasscode("bob", g.id, "x")).rejects.toThrow(/群主/)
    await expect(groups.dissolve("bob", g.id)).rejects.toThrow(/群主/)
    await expect(groups.leave("alice", g.id)).rejects.toThrow(/群主不能直接退出/)

    // 群主改名 → 会话标题同步
    await groups.rename("alice", g.id, "新名")
    expect((await store.load(g.sessionId, "alice"))?.name).toBe("新名")

    // 普通成员可退出：退出后立刻失去访问权
    await groups.leave("bob", g.id)
    expect(await store.load(g.sessionId, "bob")).toBeNull()

    // 群主转让给 bob（bob 需先重新加入）
    await groups.join("bob", "bob", { joinCode: g.joinCode, memberName: "乙" })
    await groups.transferOwner("alice", g.id, "bob")
    expect(groups.get(g.id)?.owner).toBe("bob")
    await groups.leave("alice", g.id)
    expect(await store.load(g.sessionId, "alice")).toBeNull()

    // 解散：群目录整体消失
    await groups.dissolve("bob", g.id)
    expect(existsSync(groupDir(home, g.id))).toBe(false)
    expect(groups.get(g.id)).toBeNull()
    expect(await store.load(g.sessionId, "bob")).toBeNull()
  })

  it("群注记（系统提示词用）：列出成员与发言约定，非群会话为空", async () => {
    const { groups, store } = await wire(home)
    const g = await groups.create("alice", "alice", { name: "周会", memberName: "甲", memberRole: "主持人" })
    await groups.join("bob", "bob", { joinCode: g.joinCode, memberName: "乙", memberRole: "工程师" })
    const note = groups.noteFor(g.sessionId, "bob")
    expect(note).toContain("多人会话（群）")
    expect(note).toContain("周会")
    expect(note).toContain("主持人·甲")
    expect(note).toContain("工程师·乙")
    expect(note).toContain("工程师·乙」") // 本次发起者
    expect(note).toContain("前缀")
    const single = await store.createSession("alice", "单人会话")
    expect(groups.noteFor(single.id, "alice")).toBeUndefined()
  })
})
