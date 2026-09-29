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

  // 宿主注入点（保留给宿主与测试）
  globalThis.__st = { snapshot: ensure, query, pending, handlers, timers, state };
})();
