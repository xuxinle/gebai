/**
 * 卦象数据与起卦内核（纯数据 + 纯函数，无 I/O、无随机源——随机由调用方传入）。
 *
 * 分工：本模块只管**卦怎么成**（八卦表、64 卦名、爻位语义、六十四句「生生断」、由输入算卦），
 * 「卦怎么用」（读 environment、落台账、一息一卦的闸）留在 `shengsheng.ts` 的 `divine` 工具里。
 *
 * 三张表的语义（供 `divine` 与提示词共用，改动前先读）：
 *
 * - **八卦序** 依梅花旧制：乾一 兑二 离三 震四 巽五 坎六 艮七 坤八。三爻自下而上、阳一阴零，
 *   该序恰是二进制 7..0（乾 111 … 坤 000），故「序 → 爻」无需另抄一份表。
 * - **上下卦** 上卦 = 外势（天时与粮草）、下卦 = 内势（河自身）——下卦为内，与《易》「内卦为贞」
 *   同向；**这也让爻位语义与体用判据自洽**（初二三爻在内、四五六爻在外，见下）。
 * - **爻位语义** 初=根基（验证门/仓库健康）｜二=内政（池与在途）｜三=门径（派工/续派链）｜
 *   四=外势（种子/台账/新域）｜五=君位（当前主线）｜上=极（该退役/该停的旧物）。
 * - **体用** 动爻所在之卦为「用」（事之所在），另一卦为「体」（我之势）。爻位一至三落于下卦
 *   （内势）⇒ 用卦为内势、事在本家（收/续/清池）；四至上落于上卦（外势）⇒ 事在方向库（择新/换域）。
 */

/** 三爻之一：1 = 阳，0 = 阴。 */
export type Line = 0 | 1

export interface Trigram {
  /** 梅花序（1..8）。 */
  order: number
  name: string
  symbol: string
  /** 三爻**自下而上**。 */
  lines: readonly [Line, Line, Line]
}

/** 八卦表（序即梅花旧制：乾一 兑二 离三 震四 巽五 坎六 艮七 坤八）。
 *  爻与序同源：`value = 8 - order`，其三爻即该值的三位二进制（自下而上）。 */
export const TRIGRAMS: readonly Trigram[] = (["乾", "兑", "离", "震", "巽", "坎", "艮", "坤"] as const).map((name, i) => {
  const order = i + 1
  const v = 8 - order
  return {
    order,
    name,
    symbol: ["☰", "☱", "☲", "☳", "☴", "☵", "☶", "☷"][i],
    lines: [((v >> 2) & 1) as Line, ((v >> 1) & 1) as Line, (v & 1) as Line] as const,
  }
})

/** 序 → 卦（越界回落到八卦首，调用方已保证 1..8）。 */
export const trigramOf = (order: number): Trigram => TRIGRAMS[((order - 1) % 8 + 8) % 8]

/** 三爻自下而上 → 八卦序（{@link TRIGRAMS} 的逆运算）。 */
export function trigramOrderOf(lines: readonly [Line, Line, Line]): number {
  return 8 - (lines[0] * 4 + lines[1] * 2 + lines[2])
}

/** 64 卦名表：行 = 上卦（外势），列 = 下卦（内势），行列次序皆 {@link TRIGRAMS}。 */
const HEXAGRAM_TABLE: readonly (readonly string[])[] = [
  // 下→  乾     兑     离     震     巽     坎     艮     坤
  ["乾", "履", "同人", "无妄", "姤", "讼", "遁", "否"], // 上乾
  ["夬", "兑", "革", "随", "大过", "困", "咸", "萃"], // 上兑
  ["大有", "睽", "离", "噬嗑", "鼎", "未济", "旅", "晋"], // 上离
  ["大壮", "归妹", "丰", "震", "恒", "解", "小过", "豫"], // 上震
  ["小畜", "中孚", "家人", "益", "巽", "涣", "渐", "观"], // 上巽
  ["需", "节", "既济", "屯", "井", "坎", "蹇", "比"], // 上坎
  ["大畜", "损", "贲", "颐", "蛊", "蒙", "艮", "剥"], // 上艮
  ["泰", "临", "明夷", "复", "升", "师", "谦", "坤"], // 上坤
]

/** 上下卦序 → 卦名。 */
export function hexagramName(upper: number, lower: number): string {
  return HEXAGRAM_TABLE[trigramOf(upper).order - 1][trigramOf(lower).order - 1]
}

/** 六十四句「生生断」——每卦一句，断语指向**可执行的操作族**（布/派/收/停/修门/清池/挖/拆/深观/退役），
 *  不作吉凶占断；卦只给方向域，具体落向由模型断（见 `shengsheng.md` 第五章）。 */
export const HEXAGRAM_SAYINGS: Readonly<Record<string, string>> = {
  乾: "刚健，宜布新域",
  坤: "厚载，宜收藏整备",
  屯: "初生艰难，宜小步派单",
  蒙: "未开蒙，宜先修门",
  需: "待时，宜只收不布",
  讼: "争端，宜拆合仲裁",
  师: "众力，宜并行多派",
  比: "亲附，宜续派同域",
  小畜: "蓄力，宜只收不布",
  履: "慎行，宜小步派单",
  泰: "通泰，宜深挖主线",
  否: "闭塞，宜换域另布",
  同人: "同道，宜并域共派",
  大有: "丰盛，宜多线并布",
  谦: "低处，宜稳收整备",
  豫: "豫备，宜预置清池",
  随: "顺势，宜续派不换",
  蛊: "腐坏，宜清池修裂缝",
  临: "临下，宜深观台账",
  观: "观澜，宜深观不收",
  噬嗑: "咬合，宜拆解卡点",
  贲: "文饰，宜收拾台账",
  剥: "剥落，宜退役旧物",
  复: "来复，宜收旧种重启",
  无妄: "无妄，宜照根布雨",
  大畜: "大蓄，宜囤种不布",
  颐: "养正，宜先修门",
  大过: "过甚，宜拆大单为小",
  坎: "重险，宜先修门",
  离: "附丽，宜收并既有域",
  咸: "感应，宜收账取线索",
  恒: "恒久，宜深挖一处",
  遁: "退避，宜暂停待时",
  大壮: "壮盛，宜多域并派",
  晋: "进晋，宜收成升新",
  明夷: "明入地中，宜藏器暂停",
  家人: "内治，宜清池整册",
  睽: "乖离，宜拆单合参",
  蹇: "难行，宜停派破前提",
  解: "解冻，宜续派解卡",
  损: "减损，宜收束种子",
  益: "增益，宜布雨增域",
  夬: "决断，宜定夺清池",
  姤: "相遇，宜收新线索",
  萃: "聚集，宜收合同域",
  升: "上升，宜稳步深挖",
  困: "困穷，宜省视暂停",
  井: "养而不穷，宜深挖一处",
  革: "变革，宜换域另布",
  鼎: "鼎新，宜立主线布雨",
  震: "震动，宜深观裂缝",
  艮: "止，宜停而藏器",
  渐: "渐进，宜小步续派",
  归妹: "归附，宜收并旧域",
  丰: "丰大，宜多线并收",
  旅: "旅居，宜短派探路",
  巽: "顺入，宜循线深挖",
  兑: "喜悦，宜收藏入账",
  涣: "涣散，宜清池聚拢",
  节: "节制，宜收束并发",
  中孚: "诚信，宜依门而收",
  小过: "小过，宜小步派单",
  既济: "已成，宜收藏收官",
  未济: "未成，宜停派换前提",
}

/** 六爻位语义（自下而上，与 {@link LINE_LABELS} 同序）。 */
export const LINE_SEMANTICS: readonly string[] = [
  "根基（验证门 / 仓库健康）",
  "内政（池与在途）",
  "门径（派工 / 续派链）",
  "外势（种子 / 台账 / 新域）",
  "君位（当前主线）",
  "极（该退役 / 该停的旧物）",
]

/** 爻位名（初/二/三/四/五/上），与 {@link LINE_SEMANTICS} 同序。 */
export const LINE_LABELS: readonly string[] = ["初", "二", "三", "四", "五", "上"]

/** 断语中必须命中的操作动词族（「生生断」是方向不是文学——测试据此钉住 64 句全表）。 */
export const OPERATION_FAMILIES: readonly string[] = ["布", "派", "收", "停", "修门", "清池", "挖", "拆", "深观", "退役"]

/** 起卦输入（全部实测得来，见 `divine` 工具的「感天时」一节）。 */
export interface DivineInputs {
  /** 呼吸（`control.md` 计数）。 */
  breath: number
  /** 在途条数。 */
  inflight: number
  /** 池中在跑的 worker 数。 */
  running: number
  /** 种子队列条数。 */
  seeds: number
  /** 派工序号（`control.md` 计数）。 */
  seq: number
  /** 月（1–12）。 */
  month: number
  /** 日（1–31）。 */
  day: number
  /** 时辰序（子一 … 亥十二）。 */
  hourIndex: number
  /** 掷值（0..2^32-1，由调用方取真随机；进公式并记档，故卦可复算回放）。 */
  draw: number
}

export interface DivineResult {
  /** 本卦名。 */
  name: string
  /** 上卦（外势）序号与名。 */
  upper: number
  upperName: string
  /** 下卦（内势）序号与名。 */
  lower: number
  lowerName: string
  /** 动爻（1..6，自下而上）。 */
  moving: number
  movingLabel: string
  /** 变卦名（动爻翻转后）。 */
  changed: string
  /** 体用：用卦 = 动爻所在之卦（事之所在），体卦 = 另一卦（我之势）。 */
  yong: { name: string; side: "内势" | "外势"; scope: "本家" | "方向库" }
  ti: { name: string; side: "内势" | "外势" }
  /** 该卦「生生断」。 */
  saying: string
  /** 动爻爻位语义。 */
  lineSemantics: string
}

const idx8 = (x: number): number => {
  const r = ((x % 8) + 8) % 8
  return r === 0 ? 8 : r
}

/** 六爻（自下而上）→ 上下卦序。 */
function split(lines: readonly Line[]): { upper: number; lower: number } {
  return {
    lower: trigramOrderOf([lines[0], lines[1], lines[2]]),
    upper: trigramOrderOf([lines[3], lines[4], lines[5]]),
  }
}

/** 成卦（**纯函数**：给同样的输入必得同样的卦——回放与测试都靠这一条）。
 *
 *  ```
 *  上卦序（外势）= (月 + 日 + 时辰序 + 种子数) mod 8
 *  下卦序（内势）= (呼吸 + 在途×2 + 在跑×3) mod 8
 *  动爻         = (上卦序 + 下卦序 + 派工序号 + 时辰序 + 掷) mod 6 + 1
 *  ```
 *
 *  **自由度须认清**：一息之内，上下卦的输入（呼吸/在途/在跑/种子/月日时辰）皆不变 ⇒ **本卦是这一息的
 *  「处境」，不是这一息抽出的随机方向**；唯一的随机在动爻（由「掷」承载），随呼吸递增跨息遍历。故
 *  「64 卦 = 64 个探索方向」的说法不成立——一息的可用空间是 1 本卦 + 6 变卦，长程才铺开。 */
export function buildHexagram(input: DivineInputs): DivineResult {
  const upper = idx8(input.month + input.day + input.hourIndex + input.seeds)
  const lower = idx8(input.breath + input.inflight * 2 + input.running * 3)
  const moving = (((upper + lower + input.seq + input.hourIndex + input.draw) % 6) + 6) % 6 + 1

  // 六爻自下而上：一至三爻为下卦（内势），四至上爻为上卦（外势）。
  const lines: Line[] = [...trigramOf(lower).lines, ...trigramOf(upper).lines]
  const flipped: Line[] = [...lines]
  flipped[moving - 1] = (flipped[moving - 1] === 1 ? 0 : 1) as Line
  const after = split(flipped)

  const name = hexagramName(upper, lower)
  // 动爻在内势卦（一至三爻）⇒ 事在本家；在外势卦（四至上爻）⇒ 事在方向库。
  const yongIsInner = moving <= 3
  const yong = yongIsInner
    ? { name: trigramOf(lower).name, side: "内势" as const, scope: "本家" as const }
    : { name: trigramOf(upper).name, side: "外势" as const, scope: "方向库" as const }
  const ti = yongIsInner
    ? { name: trigramOf(upper).name, side: "外势" as const }
    : { name: trigramOf(lower).name, side: "内势" as const }

  return {
    name,
    upper,
    upperName: trigramOf(upper).name,
    lower,
    lowerName: trigramOf(lower).name,
    moving,
    movingLabel: LINE_LABELS[moving - 1],
    changed: hexagramName(after.upper, after.lower),
    yong,
    ti,
    saying: HEXAGRAM_SAYINGS[name] ?? "",
    lineSemantics: LINE_SEMANTICS[moving - 1],
  }
}

/** 卦指：把「选哪个候选」交给卦——`(本卦序号-1)×6 + (动爻-1) + 掷` 再取模。
 *
 *  **为什么把「掷」一并加进取模**：若只用「本卦序号 + 动爻」取模，可选值恒为 6 个（动爻），候选数
 *  不整除 6 时次数不均，候选数 > 6 时更有候选项**永远指不到**（死区）。加上掷值（均匀覆盖全量程）
 *  后，任意候选数的可达性都完整、偏差可忽略——**卦指是随机的、确定的、可回放的**，三者同时成立。 */
export function pickCandidate(result: DivineResult, k: number, draw: number): number {
  const pool = (result.upper - 1) * 8 + (result.lower - 1)
  return ((((pool * 6 + (result.moving - 1) + draw) % k) + k) % k) + 1
}

/** 时辰序（子一 … 亥十二；子时跨 23:00–01:00）。 */
export function hourIndexOf(hours: number): number {
  return Math.floor((((hours + 1) % 24) + 24) % 24 / 2) + 1
}
