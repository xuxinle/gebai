/**
 * 执行表达式解析的**唯一实现**在 `@gebai/persona`（`src/cron.ts`），这里只做转出。
 *
 * 为什么要挪到人格包：人格定义可以在加载期声明定时任务（`DriverDef.schedules`），
 * 而加载期校验 cron 是否可解析、运行期计算档期，都必须与统一任务管理**同一套语义**。
 * 人格包的加载期校验不能反向依赖服务端，所以实现落在那一侧，本文件保留原路径转出——
 * 服务端既有调用点（`tasks.ts`、单测）路径与行为都不变。
 *
 * 不要在本文件里再写一份实现：两份 cron 语义必然分叉，而分叉的表现是
 * 「加载期接受了、运行期算出永不触发」或反之，都不会报错。
 */

export type { TaskSchedule } from "@gebai/persona"
export { isOneShotSchedule, parseSchedule } from "@gebai/persona"
