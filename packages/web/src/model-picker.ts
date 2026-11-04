/**
 * 标题栏模型配置切换器：在当前会话上选 `GEBAI_LLM_MODELS` 里的哪一条配置。
 *
 * 选择结果由**服务端持久化在会话**（chat.json 的 `modelConfig`），此后该会话的每次运行
 * （对话 / 定时任务 / 飞书桥接 / 子会话继承）都按它解析 Provider——前端只负责把下拉对齐到
 * 当前会话的选择，不把模型配置另存一份到 localStorage（两处真相必然漂移）。
 *
 * 未配置清单（没有可选条目）时整组隐藏：那时只有一份启动配置，没有可切换的东西。
 */
import type { ModelConfigSummary } from "@gebai/sdk"
import { client, getCurrentSession, setSessionCtxWindow } from "./state"
import { customSelect, toast, type SelectHandle } from "./ui"
import { configCtxWindow, configTipLines, resolveConfigName } from "./model-picker-core"

/** 宿主容器（标题栏右侧最左）。 */
let host: HTMLElement | null = null
/** 可用配置清单（启动时拉一次：清单来自进程环境，运行期不会变）。 */
let configs: ModelConfigSummary[] = []
let handle: SelectHandle | null = null

/** 清单默认配置名（第一条）：会话未选择时实际生效的就是它。 */
function defaultName(): string {
  return configs[0]?.name ?? ""
}

/** 当前生效的配置名。 */
function effectiveName(sessionModelConfig: string | undefined): string {
  return resolveConfigName(configs, sessionModelConfig)
}

/** 上下文窗口随配置切换：不同配置的 max_context 可能差数倍，圆环占比必须按实际窗口算。
 *  未声明的配置清除覆盖（回落服务端快照给的默认窗口）。 */
function applyCtxWindow(name: string): void {
  setSessionCtxWindow(configCtxWindow(configs, name))
}

/** 悬浮提示：配置的完整接入信息，不用点开下拉就能确认选的是哪一份。 */
function applyTip(name: string): void {
  if (!host) return
  const cfg = configs.find((c) => c.name === name)
  if (cfg) host.dataset.tip = configTipLines(cfg).join("\n")
  else delete host.dataset.tip
}

/** 下拉对齐当前会话（切换会话/新建会话后调用）。 */
export function syncModelPicker(): void {
  if (!host || !handle) return
  const cur = getCurrentSession()
  // 草稿态（还没有会话）：选择无处可存，隐藏入口而不是给一个按了不生效的下拉
  if (!cur) {
    host.hidden = true
    setSessionCtxWindow(0)
    return
  }
  const name = effectiveName(cur.modelConfig)
  handle.setValue(name)
  applyCtxWindow(name)
  applyTip(name)
  host.hidden = false
}

/** 用户切换：写回会话（服务端持久化），失败则把下拉恢复成服务端的实际值。 */
async function applyChoice(name: string): Promise<void> {
  const cur = getCurrentSession()
  if (!cur || name === effectiveName(cur.modelConfig)) return
  try {
    // 默认配置不写名字：清单顺序变了「默认」跟着走，会话不必因为写在里面的旧名字失配
    await client.setSessionModelConfig(cur.id, name === defaultName() ? null : name)
  } catch (err) {
    toast(`模型配置切换失败：${(err as Error).message}`)
    syncModelPicker()
    return
  }
  // 会话对象是 state 的活引用：就地更新让标题栏/后续发送立即看到新选择（其他字段不受影响）
  cur.modelConfig = name
  applyCtxWindow(name)
  applyTip(name)
  toast(`模型配置已切换为「${name}」`, "ok")
}

/** 初始化（首屏）：拉清单 → 建下拉 → 绑定会话切换。清单拉取失败静默隐藏入口（不阻断首屏）。 */
export async function initModelPicker(): Promise<void> {
  host = document.getElementById("model-picker")
  if (!host) return
  try {
    configs = await client.listModelConfigs()
  } catch {
    configs = []
  }
  if (!configs.length) {
    host.hidden = true
    return
  }
  handle = customSelect({
    options: configs.map((c) => ({ value: c.name, label: c.name })),
    value: defaultName(),
    onChange: (v) => void applyChoice(v),
  })
  host.appendChild(handle.root)
  // 会话视图切换（setCurrentSession 派发）：下拉跟着会话走——每个会话记住自己的模型配置
  document.addEventListener("gebai:session-view", syncModelPicker)
  syncModelPicker()
}

/** 供测试重置模块状态（bun test 单进程共享模块缓存）。 */
export function __resetModelPickerForTest(): void {
  host = null
  handle = null
  configs = []
}

/** 供测试注入清单（不触网）。 */
export function __setModelConfigsForTest(list: ModelConfigSummary[]): void {
  configs = list
}
