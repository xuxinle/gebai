/** 用户级偏好（REST /api/v1/prefs，**后端为真值**）。这里只放"影响服务端行为或需跨设备一致"的项；
 *  纯观感偏好（主题、浮层位置、折叠态、输入历史）仍在浏览器本地——那类没有跨设备一致的需求。 */
import type { IdleWindow } from "./task-types"

export interface UserPrefs {
  /** 默认闲时执行时间段：新建闲时任务/待办未显式指定时继承它。缺省 = 不限时段。 */
  idleWindow?: IdleWindow
  /** 最近更新时刻（毫秒）。 */
  updatedAt?: number
}
