// 霜天脚本运行时前置（`st::ui::ScriptHost` 注入到 JS 全局）。
//
// 设计目标：让"组件控制逻辑"写起来比 C++ 短得多，且 AI 能直接读懂与书写。
// 性能约束：**所有读写先在 JS 侧完成，阶段末一次性提交**（见 script_host.hpp 的性能模型）。
//
// 本文件是纯数据（一段 JS 源码），由 C++ 侧以原始字符串承载；改这里不需要动 C++。

(function () {
  'use strict';

  // ── 与宿主交换的私有状态 ────────────────────────────────────────────────
  // 宿主注入的桥：宿主以**全局函数**形式注册（`__snapshot`/`__register`/`__log`/`__now_ms`），
  // 这里取一次引用（少查几次全局表；也让"宿主到底提供了什么"在这一处一目了然）。
  const snapshotFn = globalThis.__snapshot;
  const registerFn = globalThis.__register;
  const unregisterFn = globalThis.__unregister;
  const logFn = globalThis.__log;
  const nowMsFn = globalThis.__now_ms;
  let snapshot = null;                    // 当前快照：{ nodes: {id: node}, order: [id] }
  const pending = [];                     // 待提交变更（属性/动作）
  const handlers = Object.create(null);   // 绑定 id → 处理器函数
  const timers = Object.create(null);     // 定时器 id → { fn, interval, next, repeat }
  const state = Object.create(null);      // 脚本侧状态（宿主可读写，跨执行保留）

  // ── 快照 ────────────────────────────────────────────────────────────────
  // 只在需要时重建；宿主在每次入口前会让它失效（__invalidate）。
  function ensure() {
    if (snapshot === null) snapshot = snapshotFn();
    return snapshot;
  }
  globalThis.__invalidate = function () { snapshot = null; };

  function nodeOf(target) {
    if (target == null) return null;
    // 已解析过的句柄（带 __id）
    if (typeof target === 'object' && target.__id !== undefined) return target;
    const name = String(target);
    const data = ensure();
    if (data.nodes[name]) return wrap(name, data.nodes[name]);
    // 退化为选择器查询：取首个命中
    const found = query(name, 1);
    return found.length > 0 ? found[0] : null;
  }

  const boundKeys = {};   // `元素 id|事件` → 绑定 id（同一元次重复绑定去重用）

  function wrap(id, node) {
    const handle = {
      __id: id,
      get type() { return node.type; },
      get role() { return node.role; },
      get bounds() { return node.bounds; },
      get text() { return node.text ?? (node.props ? node.props.text : undefined); },
      get value() { return node.value ?? (node.props ? node.props.value : undefined); },
      get props() { return node.props || {}; },
      get exists() { return true; },
    };
    // 属性直读：$('#x').prop('language')
    handle.prop = function (name) {
      if (name === 'text') return handle.text;
      if (name === 'value') return handle.value;
      if (node[name] !== undefined) return node[name];
      return (node.props || {})[name];
    };
    // 排队改属性（返回自身，可链式）
    handle.set = function (properties) {
      pending.push({ id, props: properties });
      // 本地同步一份，避免同一阶段内"写完再读"读到旧值
      node.props = Object.assign({}, node.props, properties);
      if (properties.text !== undefined) node.text = properties.text;
      if (properties.value !== undefined) node.value = properties.value;
      return handle;
    };
    handle.click = function () { pending.push({ id, action: 'click' }); return handle; };
    handle.focus = function () { pending.push({ id, action: 'focus' }); return handle; };
    handle.blur = function () { pending.push({ id, action: 'blur' }); return handle; };
    handle.invoke = function (action, argument) {
      pending.push({ id, action: String(action), argument: argument === undefined ? '' : String(argument) });
      return handle;
    };
    handle.find = function (selector) { return query(selector, 0, id); };
    // 事件绑定：与全局 `on(selector, event, handler)` **同一实现**（同一份 `handlers`
    // 与注册管线）——句柄上的写法更自然（脚本里 `$('#ok').on('click', fn)` 比
    // `on('#ok', 'click', fn)` 更接近直觉），而两条路名不能各写一份逻辑。
    handle.on = function (event, handler) {
      // ⚠ **同一元素同一事件重复绑定时，先解掉旧绑定**。
      //
      // 为何必要：重建式渲染（`clear()` + 重建行）下，第二次 `render()` 拿到的 has
      // **新元素但用了同一个 id**，而旧绑定还挂在宿主里（按选择器匹配）——
      // 于是一次点击会调两次处理器。
      // 实测症状极隐蔽：勾选框的 `toggle` 被调两次 = 改回原值，看起来“点了没反应”。
      const key = id + '|' + event;
      if (boundKeys[key] !== undefined) {
        try { globalThis.off(boundKeys[key]); } catch (error) { /* 已失效：忽略 */ }
      }
      const bound = globalThis.on('#' + id, event, handler);
      boundKeys[key] = bound;
      return bound;
    };
    handle.off = function (binding) { return globalThis.off(binding); };
    // —— 动态结构：与宿主桥 `ui.build`/`ui.append`/`ui.clear` 同源 ——
    //
    // 为何走宿主桥而不是纯 JS 维护一份影子树：元素是**真的 C++ 元素**
    //（有 id、能被选择器寻址、能吃控制通道）——影子树在第一次被外部查询时就露馅。
    //
    // 这三个都是**立即生效**（不走 pending 队列）：结构变化会移位置，
    // 而属性写入是批量的；两者的“什么时候看得见”不同，混在一个队列里会让
    // 后续属性写到已消失的元素上（实测：先 append 再 set 属性会静默丢）。
    handle.append = function (type, properties) {
      // 建 + 挂都走宿主桥（元素是真的 C++ 元素，见上方说明）。
      //
      // ⚠ 失败**必须抛**（而不是返回 `null`）：返回 `null` 时调用方的
      // `row.append(...)` 会报 “not a function”——那是**症状的第二层**，
      // 而真正的失败原因（“没有这个组件类型”/“父元素不存在”）已经丢了。
      // 实测踩过：排查从“点不动”一路猜到宿主桥才看到真正原因。
      const created = globalThis.__bridgeBuild(type, properties || {});
      globalThis.__bridgeAppend(id, created.id);
      snapshot = null;   // 结构变了：下次查询重建快照
      // ⚠ **不返回 `$(created.id)`**：那会走快照查询，而快照是**上一帧**的树；
      // 刚建的元素还没进去，于是拿到“空句柄”（`exists:false`）——现象就是
      // `row.append(...)` 报 `not a function`（空句柄没有 append），
      // 而根因只是“查得太早”（实测踩到，绕了几轮才定位）。
      // 这里用 `created` 自己描述的元素信息直接造句柄：调用方拿到的是**当下这个元素**，
      // 与快照新旧无关。
      return wrap(created.id, { id: created.id, type: created.type, role: undefined,
                                bounds: undefined, props: properties || {} });
    };
    handle.clear = function () {
      globalThis.__bridgeClear(id);
      snapshot = null;
      return handle;
    };
    return handle;
  }

  // 选择器查询（在快照内完成，不跨边界）。
  // `within` 指定时只在该节点子树内查找。
  function query(selector, limit, within) {
    const data = ensure();
    const text = String(selector).trim();
    const results = [];
    let rootId = within === undefined ? null : within;
    const inSubtree = (element) => {
      if (rootId === null) return true;
      for (let current = element; current != null; current = data.nodes[current] ? data.nodes[current].parent : null) {
        if (current === rootId) return true;
      }
      return false;
    };
    // 选择器语法（够用且可组合，与常见 CSS 习惯一致）：
    //   '#id'                 按 id
    //   'Type'                按组件类型（大小写不敏感）
    //   'Type[prop=v]'        属性相等（值可不加引号）
    //   'Type[prop^=v]'       前缀      'Type[prop$=v]' 后缀      'Type[prop*=v]' 包含
    // 类型可省略（'[id^=tab-]' 即"任意组件，id 以 tab- 开头"）。
    const attr = text.match(/^([A-Za-z_][\w]*)?\[([\w-]+)\s*(=|\^=|\$=|\*=)\s*"?([^"\]]*)"?\]$/);
    const byId = text.startsWith('#');
    const wantType = (attr && attr[1]) || (!byId && !attr ? text : null);
    const wantProp = attr ? attr[2] : null;
    const operator = attr ? attr[3] : null;
    const wantValue = attr ? attr[4] : null;

    function propMatches(node, name, op, expected) {
      const actual = node[name] !== undefined ? node[name] : (node.props || {})[name];
      if (actual === undefined || actual === null) return false;
      const value = String(actual);
      if (op === '=') return value === expected;
      if (op === '^=') return value.startsWith(expected);
      if (op === '$=') return value.endsWith(expected);
      return value.indexOf(expected) >= 0;  // '*='
    }

    for (const id of data.order) {
      const node = data.nodes[id];
      if (!node || !inSubtree(id)) continue;
      if (byId && id !== text.slice(1)) continue;
      if (wantType && String(node.type).toLowerCase() !== wantType.toLowerCase()) continue;
      if (wantProp && !propMatches(node, wantProp, operator, wantValue)) continue;
      results.push(wrap(id, node));
      if (limit > 0 && results.length >= limit) break;
    }
    return results;
  }

  // ── 对外 API ────────────────────────────────────────────────────────────
  globalThis.$ = function (selector) {
    const found = query(selector, 1);
    if (found.length > 0) return found[0];
    // 未命中：返回一个"空句柄"，读属性得 undefined、写操作被忽略——
    // 让脚本不必到处判空（AI 写的脚本尤其容易漏判），但 .exists 可显式检查。
    // 未命中：读属性得 undefined、写操作被忽略（脚本不必到处判空），
    // 但**每一次写操作都记一条日志**——拼错选择器是最常见的脚本错误，
    // 静默无效会让"改了没反应"变成难查问题（实践踩过：`$('brand')` 少了 `#`，
    // 裸词按类型匹配，于是整个 set 被静默丢弃）。
    const warn = () => {
      logFn('[警告] 未命中（读属性得到 undefined，写操作被忽略）: ' + String(selector)
            + ' —— 按 id 选要写 #id，如 #' + String(selector));
    };
    const missing = {
      __id: null, exists: false, type: undefined, role: undefined, bounds: undefined,
      text: undefined, value: undefined, props: {},
      prop: () => undefined,
      set: () => { warn(); return missing; },
      click: () => { warn(); return missing; },
      focus: () => { warn(); return missing; },
      blur: () => { warn(); return missing; },
      invoke: () => { warn(); return missing; },
      find: () => [],
    };
    return missing;
  };
  globalThis.$$ = function (selector, limit) { return query(selector, limit === undefined ? 0 : limit); };
  globalThis.count = function (selector) { return query(selector, 0).length; };
  globalThis.tree = function () { return ensure(); };

  // 事件绑定：处理器留在 JS 侧，宿主只存"选择器 + 事件 + 绑定 id"。
  globalThis.on = function (selector, event, handler) {
    if (typeof handler !== 'function') throw new TypeError('on(selector, event, handler)：handler 必须是函数');
    const id = registerFn(String(selector), String(event), String(handler));
    handlers[id] = handler;
    return id;
  };
  globalThis.off = function (id) {
    delete handlers[id];
    return unregisterFn(String(id));
  };
  globalThis.__dispatch = function (id, event) {
    const handler = handlers[id];
    if (typeof handler !== 'function') return false;
    handler(event);
    return true;
  };

  // 定时器：宿主按帧推进，到期回调在 JS 侧执行。
  let timerSeq = 0;
  globalThis.every = function (intervalMs, fn) {
    const id = 't' + (++timerSeq);
    timers[id] = { fn, interval: intervalMs, next: nowMsFn() + intervalMs, repeat: true };
    return id;
  };
  globalThis.after = function (delayMs, fn) {
    const id = 'a' + (++timerSeq);
    timers[id] = { fn, interval: delayMs, next: nowMsFn() + delayMs, repeat: false };
    return id;
  };
  globalThis.cancel_timer = function (id) { delete timers[id]; };
  // 宿主调用：推进到期定时器，返回触发个数
  globalThis.__run_timers = function (nowMs) {
    let fired = 0;
    for (const id of Object.keys(timers)) {
      const timer = timers[id];
      if (nowMs < timer.next) continue;
      if (timer.repeat) timer.next = nowMs + timer.interval;
      else delete timers[id];
      ++fired;
      try { timer.fn(nowMs); } catch (error) { logFn('定时器异常: ' + error); }
    }
    return fired;
  };

  // 状态：跨执行保留，宿主可读写。
  globalThis.state = state;
  globalThis.log = function () { logFn(Array.prototype.map.call(arguments, fmt).join(' ')); };
  function fmt(value) {
    if (typeof value === 'string') return value;
    try { return JSON.stringify(value); } catch (e) { return String(value); }
  }
  globalThis.now = function () { return nowMsFn(); };

  // 变更集提交：宿主在阶段末调用一次，取走队列。
  globalThis.__take_changes = function () { return pending.splice(0, pending.length); };

  // ── 重载支持（宿主驱动；应用脚本不要自己调）─────────────────────────
  //
  // 为什么需要 `__reset_script_state`：引擎是**同一个**，页面脚本每次装载都在新
  // 作用域里重建闭包，而旧闭包仍被 `handlers`/`timers` 两张表强引用（append-only）。
  // 不清的后果都是实测级：① 一次点击跑两遍旧处理器（表现为“点了没反应/值又弹回去”）；
  // ② 每个定时器被装出第二份，重载几次后 CPU 与内存一起涨。
  //
  // 清理顺序按**谁引用谁**排：定时器 → 处理器 → 绑定键 → 状态 → 快照。
  //
  // ⚠ 不清 `app.*`：那些是**宿主函数**（`expose` 时由桥挂上去的 C++ 回调），
  // 桥只在启动时挂一次——删了就没有谁再挂回来（实测：重载后第一行 `app.tasks.list()`
  // 就报 `TypeError: not a function`，而源头是“清得太干净”）。会泄漏的是**应用自己的
  // 闭包**（handlers/timers 两张表强引用的），上面已经清完。
  globalThis.__reset_script_state = function () {
    for (const id of Object.keys(timers)) delete timers[id];
    for (const id of Object.keys(handlers)) delete handlers[id];
    for (const key of Object.keys(boundKeys)) delete boundKeys[key];
    for (const key of Object.keys(state)) delete state[key];
    pending.length = 0;
    snapshot = null;
    return true;
  };

  // `hot_reload()`：脚本里的**一句话重载**（开发时“改完在窗口里点一下”）。
  //
  // 必须由宿主 expose（`app.hot.reload`）而不是这里自己读文件：脚本层没有
  // 文件/进程能力（见 `st/ext/script.hpp` 的安全姿态），读文件、重建树、重装脚本
  // 全是宿主的事。这里只负责“叫一声”，并在宿主没装时**说清楚**（不静默 false）。
  globalThis.hot_reload = function () {
    if (typeof globalThis.app !== 'object' || globalThis.app === null ||
        typeof globalThis.app.hot === 'undefined' ||
        typeof globalThis.app.hot.reload !== 'function') {
      logFn('[警告] hot_reload() 不可用：宿主未装载 HotReload（或未 expose app.hot.reload）');
      return false;
    }
    return globalThis.app.hot.reload() === true;
  };

  // 宿主注入点（保留给宿主与测试）
  globalThis.__st = { snapshot: ensure, query, pending, handlers, timers, state };
})();
