// 声明式 UI 运行时（JS 宿主，与 script_api.js 共存于同一上下文）。
//
// 设计（docs/declarative.md §6）：
// - VDOM 与重组器纯 JS：diff 在 JS 内完成，落地时每帧最多一次跨界（批量提交）；
// - 树操作走宿主窄桥 `__d_create/__d_apply/__d_set_root/__d_mount/__d_unmount/__d_move`；
// - 事件回调复用 script_api 的 on/off 管线（绑定 id 记在 VNode 上，卸载时反注册）；
// - 属性写入复用 pending 变更集（$('#id').set 同一队列，阶段末统一提交）。
//
// 与 C++ 宿主（st::ui::dsl）同一语义规范：位置对齐复用、条件裁剪、单根语义。

(function () {
  'use strict';

  // ── 状态系统（≈ mutableStateOf / @State）────────────────────────────────
  // 读时登记当前重组作用域，写时标脏（下一帧统一重组——帧对齐合并）。

  let currentScope = null;        // 当前正在 build 的作用域
  const scopes = [];              // 全部已挂载作用域
  let dirty = true;               // 是否有待重组（v1 单根：任一状态写即全树重组候选）
  let rootVdom = null;            // 当前根 VNode
  let mounted = false;

  function enterScope(scope) { currentScope = scope; }
  function exitScope() { currentScope = null; }

  function readDependency(cell) {
    if (currentScope !== null && cell._subs.indexOf(currentScope) < 0) {
      cell._subs.push(currentScope);
    }
  }

  function state(initial, onWrite) {
    const cell = {
      _value: initial,
      _subs: [],
      get value() {
        readDependency(cell);
        return cell._value;
      },
      set value(next) {
        if (next === cell._value) return;
        cell._value = next;
        if (typeof onWrite === 'function') onWrite(next);   // 写穿透（`usePersisted` 用）
        markDirty(cell);
      },
    };
    return cell;
  }

  /// State 工厂别名（hooks 内部用；与 `useState` 同一实现）。
  const state_cell = state;

  function markDirty() {
    dirty = true;
    if (typeof requestRepaint === 'function') requestRepaint();
  }

  // ── VDOM ────────────────────────────────────────────────────────────────
  // VNode：{ type, props, key, children: [], el: id|null }
  // 描述性节点；`el` 是真值树上元素的 id（挂载后回填，diff 的对齐锚点）。
  // 事件绑定不在 VNode 上（回调闭包每帧重建）——由 `eventHolders` 按**元素 id** 持有，
  // 见「事件绑定（绑定身份 = 元素）」一节。

  function vnode(type, props, children) {
    return { type, props: props || {}, key: props && props.key || '', children: children || [], el: null };
  }

  // ── 声明函数（build() 里用；等价 C++ 侧的 row/column/text/button…）──────

  function row(props, children) { return vnode('Row', props, children); }
  function column(props, children) { return vnode('Column', props, children); }
  function text(content, props) {
    const node = vnode('Text', props, []);
    node.text = content;                    // 字符串或 () => string
    return node;
  }
  function button(label, onClick, props) {
    const node = vnode('Button', props, []);
    node.label = label;
    node.onClick = onClick;
    return node;
  }
  function checkbox(label, checked, onChange, props) {
    const node = vnode('Checkbox', props, []);
    node.label = label;
    node.checked = checked;
    node.onChange = onChange;
    return node;
  }
  function switchToggle(checked, onChange, props) {
    const node = vnode('Switch', props, []);
    node.checked = checked;
    node.onChange = onChange;
    return node;
  }
  function input(value, onInput, props) {
    const node = vnode('Input', props, []);
    node.value = value;
    node.onInput = onInput;
    return node;
  }
  function progress(value, props) {
    const node = vnode('ProgressBar', props, []);
    node.value = value;
    return node;
  }
  function badge(content, props) {
    const node = vnode('Badge', props, []);
    node.text = content;
    return node;
  }
  function heading(content, level, props) {
    const node = vnode('Heading', props, []);
    node.text = content;
    node.level = level || 1;
    return node;
  }
  function card(props, children) { return vnode('Card', props, children); }
  function divider(props) { return vnode('Divider', props, []); }
  function spacer(size, props) {
    const node = vnode('Spacer', props, []);
    node.size = size || 0;
    return node;
  }

  // ── ArkTS 风格：链式修饰符（≈ ArkUI 属性链 / 事件链）──────────────────────
  //
  // 为什么用链式而不是 `struct X { @State n = 0; build() { ... } }`：
  // 那段块级语法在本 JS 运行时里**不是合法表达式**——要支持它就得写一个真正的
  // 解析器（切块、匹配花括号、重写 `this.n`），而正则级切块在嵌套 struct/字符串里
  // 会误判，失败模式是静默产出垃圾代码。链式修饰符用合法 JS 表达同一心智模型：
  // 「构造 → 逐项修饰属性 → 挂事件」，与 ArkUI 的 `.width().onClick()` 同形，
  // 且 AI 与人都能直接读写、报错能指到行。
  //
  // 命名约定与 ArkTS 对齐：**大写组件名 + 链式修饰**（`Text('hi').padding(8)`）；
  // 小写声明函数是 compose 风格（`text('hi')`）——两套产出同一个 VNode。

  /// 链式修饰器（内部持真 VNode；容器侧归一化时取 `._vnode`）。
  function chainOf(node) {
    const chain = {
      _vnode: node,
      /// 通用属性（任意组件属性面/布局属性名）
      prop: function (name, value) { node.props[name] = value; return chain; },
      padding: function (value) { return chain.prop('padding', value); },
      margin: function (value) { return chain.prop('margin', value); },
      gap: function (value) { return chain.prop('gap', value); },
      width: function (value) { return chain.prop('width', value); },
      height: function (value) { return chain.prop('height', value); },
      grow: function () { return chain.prop('grow', true); },
      radius: function (value) { return chain.prop('radius', value); },
      key: function (value) { node.key = String(value); return chain; },
      /// 事件（≈ .onClick()）
      onClick: function (fn) { node.onClick = fn; return chain; },
      onChange: function (fn) { node.onChange = fn; return chain; },
      onInput: function (fn) { node.onInput = fn; return chain; },
      /// 子节点（容器）：追加
      append: function (kids) { node.children = node.children.concat(normalizeAll(kids)); return chain; },
      /// 取值回到 VNode
      build: function () { return node; },
    };
    return chain;
  }

  /// 归一化：链对象 → 真 VNode（容器收到子节点时统一展开）。
  function normalize(node) {
    if (node == null) return node;
    return node._vnode !== undefined ? node._vnode : node;
  }
  /// 归一化 + 片段展开：**数组**与 `__fragment` 容器都摊平到父级
  /// （≈ ArkUI：ForEach 直接当作兄弟列表用，不多套一层）。
  ///
  /// 为何数组也要摊平：`forEach()` 的返回值是子节点列表，AI 与人都常写成
  /// `column({}, [text('头'), forEach(...)])`（把列表结果直接塞进 kids 数组）。
  /// 只认 `__fragment` 的话，这个数组会被当成一个 VNode——`type` 是 `undefined`，
  /// 创建环节报一行错就整段消失（**静默失效**：界面少一块而不崩）。
  function normalizeAll(kids) {
    const out = [];
    /// 递归摊平：数组 = 兄弟列表；`__fragment` 容器 = 它的 children。
    function flatten(raw) {
      if (raw == null) return;
      if (Array.isArray(raw)) {
        for (const inner of raw) flatten(inner);
        return;
      }
      const node = normalize(raw);
      if (node == null) return;
      if (node.type === '__fragment') {
        flatten(node.children);
        return;
      }
      out.push(node);
    }
    flatten(kids);
    return out;
  }

  // ── For 控制流（数据数组 → 子 VNode；key 对齐复用）──────────────────────

  function forEach(items, keyFn, itemFn) {
    // v1：展开为子节点（key 记在节点上；按 key 复用待接——见 BACKLOG）
    return normalizeAll(items.map(function (item) {
      const child = normalize(itemFn(item));
      child.key = String(keyFn(item));
      return child;
    }));
  }

  // ── ArkTS 风格组件构造（大写名 + 链式修饰）─────────────────────────────
  //
  // `Text('hi').padding(8)` / `Row().gap(12).append([...])`——与 ArkUI 书写习惯一致。
  // 与 compose 风格（小写 text/row）产出同一 VNode，可混用。

  function chainContainer(type, props, kids) {
    return chainOf(vnode(type, props || {}, normalizeAll(kids || [])));
  }

  const ArkUI = {
    Text: function (content, props) {
      const node = vnode('Text', props, []);
      node.text = content === undefined ? '' : content;
      return chainOf(node);
    },
    Heading: function (content, level) {
      const node = vnode('Heading', {}, []);
      node.text = content === undefined ? '' : content;
      node.level = level || 1;
      return chainOf(node);
    },
    Button: function (label, onClick) {
      const node = vnode('Button', {}, []);
      node.label = label === undefined ? '' : label;
      // ArkUI 习惯：Button('文本', () => {...}) —— 第二参可为回调
      if (typeof onClick === 'function') node.onClick = onClick;
      return chainOf(node);
    },
    Card: function (kids) { return chainContainer('Card', {}, kids); },
    Row: function (kids) { return chainContainer('Row', {}, kids); },
    Column: function (kids) { return chainContainer('Column', {}, kids); },
    Checkbox: function (label, checked, onChange) {
      const node = vnode('Checkbox', {}, []);
      node.label = label === undefined ? '' : label;
      node.checked = !!checked;
      if (typeof onChange === 'function') node.onChange = onChange;
      return chainOf(node);
    },
    Switch: function (checked, onChange) {
      const node = vnode('Switch', {}, []);
      node.checked = !!checked;
      if (typeof onChange === 'function') node.onChange = onChange;
      return chainOf(node);
    },
    Input: function (value, onInput) {
      const node = vnode('Input', {}, []);
      node.value = value === undefined ? '' : value;
      if (typeof onInput === 'function') node.onInput = onInput;
      return chainOf(node);
    },
    Progress: function (value) {
      const node = vnode('ProgressBar', {}, []);
      node.value = value;
      return chainOf(node);
    },
    Badge: function (content) {
      const node = vnode('Badge', {}, []);
      node.text = content === undefined ? '' : content;
      return chainOf(node);
    },
    Divider: function () { return chainOf(vnode('Divider', {}, [])); },
    Spacer: function (size) {
      const node = vnode('Spacer', {}, []);
      node.size = size || 0;
      return chainOf(node);
    },
    /// ForEach（≈ ArkUI 的 ForEach）：数据数组 → 子节点，key 取业务身份
    ForEach: function (items, keyFn, itemFn) {
      return chainOf({ type: '__fragment', props: {}, key: '', children: forEach(items, keyFn, itemFn), el: null });
    },
    /// Slider（拖拽回调）
    Slider: function (value, onChange) {
      const node = vnode('Slider', {}, []);
      node.value = value;
      if (typeof onChange === 'function') node.onChange = onChange;
      return chainOf(node);
    },
  };

  // ── 重组器（diff + 落地）───────────────────────────────────────────────

  // 属性批量队列：重组末尾一次跨界（__d_apply_batch）——不依赖快照（新建元素
  // 还没挂树、快照里没有它）；也不逐属性往返。
  const applyQueue = [];
  function queueApply(id, props) {
    if (id == null || props === undefined) return;
    const keys = Object.keys(props);
    if (keys.length === 0) return;
    applyQueue.push({ id: id, props: props });
  }
  function flushApply() {
    if (applyQueue.length === 0) return;
    try { __d_apply_batch(applyQueue); } catch (error) { log('[declarative] 属性批量失败: ' + error); }
    applyQueue.length = 0;
  }

  // ── 事件绑定（绑定身份 = **元素**，不是每帧重建的 VNode）──────────────
  //
  // `eventHolders[el] = { node, kinds, bindings }`：宿主注册的处理器只做一件事——
  // 把事件转给 `holder.node` 上**当前这一帧**的回调。
  //
  // 为什么要这层间接：回调闭包每帧重建（`() => { n.value++ }` 是 build 里的新函数对象），
  // 若按「回调变了就重绑」实现，等价于**每帧把整棵树的事件全部注销重绑**
  // （实测：三帧的绑定 id 是 b1..b3 → b4..b6 → b7..b9，codeeditor 整页声明式每帧在付这笔钱）。
  // 间接一层之后：绑定只在**该元素第一次出现事件回调时**登记一次、卸载时反注册
  // ——复用帧零事件成本，而「回调是最新一帧的」由 `holder.node` 的更新保证。
  const eventHolders = new Map();   // 元素 id → { node, kinds, bindings }

  /// 事件转发体：读 `holder.node`（本帧的 VNode）上的回调。
  function handlerFor(holder, kind) {
    return function (event) {
      const node = holder.node;
      if (node == null) return;
      if (kind === 'click') {
        if (node.onClick) node.onClick();
      } else if (kind === 'change') {
        // 观察者在元素处理之后：此刻 checked 已是新值（属性面可读回）
        if (node.onChange) node.onChange(!!($('#' + node.el).prop('checked')));
      } else if (kind === 'input') {
        // TextInput 事件带 text 字段（尚未落到元素）；缺失时读属性面
        const fresh = event && event.text !== undefined
            ? event.text
            : String($('#' + node.el).prop('value') || '');
        if (node.onInput) node.onInput(fresh);
      }
    };
  }

  function bindKind(holder, kind, event) {
    if (holder.kinds[kind]) return;   // 已登记：不重绑
    holder.kinds[kind] = true;
    holder.bindings.push(on('#' + holder.node.el, event, handlerFor(holder, kind)));
  }

  function ensureEventBinding(node) {
    if (node.el == null) return;
    let holder = eventHolders.get(node.el);
    if (holder === undefined) {
      holder = { node: node, kinds: Object.create(null), bindings: [] };
      eventHolders.set(node.el, holder);
    }
    holder.node = node;   // 间接层的「指针更新」：本帧回调生效
    // 反向验证挂点：把下一行换成「先 releaseEvents(node) 再无条件 bind」，
    // `declarative_event_bindings_survive_reuse_frames` 立即红（绑定 id 每帧都换）。
    // 按需登记：每种事件只在这一帧确实有回调、且尚未登记时绑一次。
    // 为何不一次绑齐三种：宿主的事件分发要扫全部绑定（选择器解析 + 匹配），
    // 白绑的条目会让每个事件多付一次无意义的匹配。
    if (node.onClick) bindKind(holder, 'click', 'click');
    if (node.onChange) bindKind(holder, 'change', 'click');
    if (node.onInput) bindKind(holder, 'input', 'input');
  }

  /// 反注册某元素的事件绑定（卸载时调；holder 随之丢弃）。
  function releaseEvents(node) {
    if (node == null || node.el == null) return;
    const holder = eventHolders.get(node.el);
    if (holder === undefined) return;
    for (const id of holder.bindings) off(id);
    eventHolders.delete(node.el);
  }

  function isContainer(node) { return node.type === 'Row' || node.type === 'Column' || node.type === 'Card'; }

  function domType(node) {
    if (node.type === 'Row' || node.type === 'Column') return 'Panel';
    return node.type;
  }

  // VNode → 属性包（含布局语义 direction；文本/状态值在调用侧补充）
  function propsToApply(node) {
    const apply = {};
    const props = node.props || {};
    for (const name of Object.keys(props)) {
      if (name === 'key' || name === 'children') continue;
      apply[name] = props[name];
    }
    if (node.type === 'Row') apply.direction = 'row';
    if (node.type === 'Column') apply.direction = 'column';
    if (node.text !== undefined) apply.text = typeof node.text === 'function' ? node.text() : node.text;
    if (node.label !== undefined) apply.label = node.label;
    if (node.checked !== undefined) apply.checked = !!node.checked;
    if (node.value !== undefined) apply.value = node.value;
    if (node.level !== undefined) apply.level = node.level;
    if (node.size !== undefined) { apply.width = node.size; apply.height = node.size; }
    return apply;
  }

  // 同位对齐 diff：parent 为真值树父元素 id（null = 根槽位）
  function reconcileChildren(parentId, oldChildren, newChildren, stats) {
    // 归一化（链对象取 `_vnode`；ForEach 片段已摊平——见 normalizeAll）
    const fresh = normalizeAll(newChildren);
    const stale = normalizeAll(oldChildren);

    // ── key 对齐（优先）──────────────────────────────────────────────
    //
    // 语义同 `List::sync_items`：**有 key 的节点按 key 匹配**（身份跟 key 走，
    // 顺序变化不换元素、id/事件绑定保持）；无 key 退回位置对齐。
    // 为什么关键：列表插入/删除/重排时，纯位置对齐会把"第 2 项的 id"换成别的内容，
    // 外部按 id 引用（选择器/协议）立即错位——这正是 `sync_items` 当年被发明的原因。
    const keyed = new Map();       // key → { node, index }
    for (let index = 0; index < stale.length; ++index) {
      const node = stale[index];
      if (node !== null && node.key) keyed.set(node.key, index);
    }
    const consumed = new Array(stale.length).fill(false);
    const matches = new Array(fresh.length).fill(null);   // fresh[i] ← 旧的同位/同 key 节点
    // 第一遍：按 key 匹配
    for (let index = 0; index < fresh.length; ++index) {
      const node = fresh[index];
      if (!node.key) continue;
      const oldIndex = keyed.get(node.key);
      if (oldIndex !== undefined && !consumed[oldIndex] && stale[oldIndex].type === node.type) {
        matches[index] = stale[oldIndex];
        consumed[oldIndex] = true;
      }
    }
    // 第二遍：无 key（或 key 未命中）按位置对齐剩余旧节点
    let cursor = 0;
    for (let index = 0; index < fresh.length; ++index) {
      if (matches[index] !== null) continue;
      while (cursor < stale.length && consumed[cursor]) ++cursor;
      if (cursor < stale.length) {
        matches[index] = stale[cursor];
        consumed[cursor] = true;
        ++cursor;
      }
    }
    // 落地：逐位置对（含复用/替换/新建）
    for (let index = 0; index < fresh.length; ++index) {
      reconcileAt(parentId, index, matches[index], fresh[index], stats);
    }
    // 顺序修正：复用元素保持旧树位置，key 重排后顺序会错——按 VNode 序重排。
    // 只在存在 key 时做（无 key 的列表顺序由位置对齐保证，无需额外跨界）。
    if (fresh.some(function (node) { return !!node.key; })) {
      for (let index = 0; index < fresh.length; ++index) {
        const node = fresh[index];
        if (node.el) __d_move(node.el, parentId, index);   // 已在位则 C++ 侧直接返回
      }
    }
    // 裁剪：未被任何新节点认领的旧节点 → 卸载
    for (let index = 0; index < stale.length; ++index) {
      if (!consumed[index]) unmountVNode(stale[index], stats);
    }
  }

  function reconcileAt(parentId, index, oldNode, newNode, stats) {
    const sameType = oldNode !== null && oldNode.type === newNode.type;
    if (sameType) {
      // 复用：更新 props/事件
      newNode.el = oldNode.el;
      const apply = propsToApply(newNode);
      // 只排变更过的属性（与旧帧已应用集合比较）
      const prev = oldNode._applied || {};
      const changed = {};
      for (const key of Object.keys(apply)) {
        if (apply[key] !== prev[key]) changed[key] = apply[key];
      }
      if (Object.keys(changed).length > 0) {
        queueApply(newNode.el, changed);
        stats.props += Object.keys(changed).length;
        newNode._applied = apply;
      } else {
        newNode._applied = prev;
      }
      // 事件绑定按元素身份持有（`eventHolders`）：这里只更新间接层的指针，不重绑。
      ensureEventBinding(newNode);
      // 容器：递归子节点
      if (isContainer(newNode)) reconcileChildren(newNode.el, oldNode.children, newNode.children, stats);
    } else {
      // 替换：卸旧建新
      if (oldNode !== null) unmountVNode(oldNode, stats);
      mountVNode(parentId, index, newNode, stats);
    }
  }

  function mountVNode(parentId, index, node, stats) {
    const type = domType(node);
    let created = null;
    try { created = __d_create(type); } catch (error) { log('[declarative] 创建失败 ' + type + ': ' + error); return; }
    if (created == null) return;
    ++stats.created;
    // 属性（含 direction）：挂树后按正式 id 批量应用（__d_apply_batch 统一路径）
    const apply = propsToApply(node);
    if (node.key) apply.key = node.key;
    node._applied = apply;
    // 挂树（拿正式 id）。parentId 为 null = 根槽位：宿主可能返回**已有根 id**
    // （子树形态首帧），届时不再新建——见 __d_set_root 的宿主槽位分支。
    let formalId = null;
    try {
      formalId = parentId === null ? __d_set_root(created) : __d_mount(parentId, index, created);
    } catch (error) {
      log('[declarative] 挂载失败 ' + type + ': ' + error);
      return;
    }
    node.el = formalId;
    queueApply(formalId, apply);
    // 事件
    ensureEventBinding(node);
    // 子节点（归一化：链对象取 `_vnode`；ForEach 片段摊平后索引连续）
    if (isContainer(node)) {
      const kids = normalizeAll(node.children);
      for (let i = 0; i < kids.length; ++i) {
        mountVNode(node.el, i, kids[i], stats);
      }
    }
  }

  function unmountVNode(node, stats) {
    if (node == null || node.el == null) return;
    releaseEvents(node);
    if (isContainer(node)) {
      const kids = normalizeAll(node.children);   // 片段已摊平：递归用摊平后的列表
      for (const child of kids) unmountVNode(child, stats);
    }
    __d_unmount(node.el);
    ++stats.removed;
    node.el = null;
  }

  // ── 对外 API（挂载与重组）──────────────────────────────────────────────

  // —─ 对外暴露（全局：build 闭包里直接用，AI 书写成本最低）──
  const api = {
    row, column, text, button, checkbox, switchToggle, input, progress, badge,
    heading, card, divider, spacer, forEach,
  };
  for (const name of Object.keys(api)) globalThis[name] = api[name];
  // switch 是保留字——暴露为 switchView（与 switchToggle 同义）
  globalThis.switchView = switchToggle;
  // ArkTS 风格组件（大写名 + 链式修饰）：Text/Button/Row/Column/Card/ForEach/…
  for (const name of Object.keys(ArkUI)) globalThis[name] = ArkUI[name];

  globalThis.compose = function (name, buildFn) {
    // 重新挂载 = 新一层界面：先卸掉上一层（否则两次 compose 的元素树叠在一起——
    // 旧界面不会自己消失，而新界面的根只是「又挂了一个根」）。
    if (rootVdom !== null) {
      unmountVNode(rootVdom, { created: 0, removed: 0, props: 0 });
      rootVdom = null;
    }
    // 卸载清理：effect 的清理函数先跑（≈ 组件卸载），hook 槽与事件登记随之清空。
    // 不清的话：同一会话里再 compose 会**累加** effect 槽——被卸掉的界面里的
    // effect 清理永不被调、且新界面的 hook 从第 N+1 个槽往后取（槽位错位）。
    globalThis.__d_dispose_effects();
    hookSlots.length = 0;
    scopes.length = 0;
    eventHolders.clear();   // 事件登记随场景走（旧树的已在 unmountVNode 里逐个反注册）
    const scope = { name: name, build: buildFn, alive: true };
    scopes.push(scope);
    mounted = true;
    dirty = true;
    globalThis.__d_reconcile();
    return scope.name;   // 字符串可序列化（函数/对象会被引擎拒收）
  };

  globalThis.useState = state;    // build 闭包内可用（与 C++ State<T> 同语义）

  // ── hooks 槽位（按调用点序号对齐）──────────────────────────────────────
  //
  // `useResource` / `useMemo` / `useEffect` / `useRef` / `usePersisted` **共用一个游标**：
  // 同一 build 里第 N 个 hook 调用 ↔ 第 N 个槽，跨重组复用（与 C++ 侧 `hook_index` 同规则）。
  // 后果（同 React Hooks）：hook 不能写在条件分支里——调用点顺序一变，槽就错位
  // （错位不是崩溃而是**静默串味**：第 2 个 hook 拿到第 1 个的缓存值）。
  const hookSlots = [];
  let hookCursor = 0;

  /// 取/建当前调用点的槽（对象由调用方按需补字段）。
  function hookSlot(kind) {
    const index = hookCursor++;
    let slot = hookSlots[index];
    if (slot === undefined || slot.kind !== kind) {
      // 槽类型变了 = 调用点顺序变了：新建（旧缓存丢弃，不静默串味）
      slot = { kind: kind };
      hookSlots[index] = slot;
    }
    return slot;
  }

  // ── 依赖指纹（memo/effect 用）：值按 JSON 比较 ───────────────────────────
  //
  // 与 C++ 侧 `deps_signature`（指针 + 写版本）同一语义：**依赖没变就不重算**。
  // JS 侧没有「状态版本」可拿（值是任意 JS 值），退化为值比较——
  // `JSON.stringify` 对 undefined/函数/循环引用不成立，故：不可序列化的依赖
  // 一律视为「变了」（保守重算，宁可多算不漏算）。
  function depsKey(deps) {
    const list = deps === undefined ? [] : (Array.isArray(deps) ? deps : [deps]);
    try {
      return JSON.stringify(list.map(function (item) { return item === undefined ? 0 : item; }));
    } catch (error) {
      return 'vary:' + Math.random();   // 循环引用等：每次都当变了
    }
  }

  // ── useMemo：依赖未变则复用上次结果 ─────────────────────────────────────
  function useMemo(factory, deps) {
    const slot = hookSlot('memo');
    const key = depsKey(deps);
    if (slot.key !== key) {
      slot.key = key;
      slot.value = factory();
    }
    return slot.value;
  }

  // ── useEffect：依赖变化时执行一次（返回清理函数）──────────────────────
  //
  // 执行时机是**重组结束之后**（宿主 `tick()` 末尾调 `__d_run_effects`）：
  // 副作用里写状态属于「连锁写」，隔帧生效——在 build 中途执行的话，
  // 那次写会被同帧的 dirty 清理吞掉（界面停在旧值）。
  const pendingEffects = [];   // 本帧待执行（重跑 build 时重填）
  let effectSeq = 0;           // 登记序号（执行顺序 = 声明顺序）

  function useEffect(fn, deps) {
    const slot = hookSlot('effect');
    const key = depsKey(deps);
    if (slot.key === key && slot.ran) return;   // 依赖未变且已跑过：跳过
    slot.key = key;
    pendingEffects.push({ seq: effectSeq++, slot: slot, fn: fn });
  }

  // 宿主在重组末尾调：先跑旧清理，再跑新体（体返回函数则记为下次的清理）。
  //
  // 与 C++ 侧 `Composer::reconcile` 末尾的 `run_pending_effects` 严格同序：
  // **重组结束后**才跑（副作用里写状态 = 连锁写，隔帧生效）。
  function runPendingEffects() {
    if (pendingEffects.length === 0) return 0;
    const jobs = pendingEffects.splice(0, pendingEffects.length);
    jobs.sort(function (a, b) { return a.seq - b.seq; });
    let ran = 0;
    for (const job of jobs) {
      try {
        if (job.slot.cleanup) job.slot.cleanup();
        job.slot.cleanup = null;
        const result = job.fn();
        if (typeof result === 'function') job.slot.cleanup = result;
        job.slot.ran = true;
        ++ran;
      } catch (error) {
        log('[declarative] effect 异常: ' + error);
      }
    }
    return ran;
  }

  // 宿主/外部手动推进（`tick` 之外单独跑 effect 的场合）。
  globalThis.__d_run_effects = function () {
    return runPendingEffects();
  };

  // ── 桥名纪律（写这里时最容易踩的坑）────────────────────────────────────
  //
  // 宿主窄桥以 `__d_` 开头注入：`__d_create/__d_set_root/__d_mount/__d_unmount/`
  // `__d_move/__d_apply_batch/__d_slot_root/__d_clear_slot`；本文件以**同一个全局命名空间**
  // 暴露运行时入口（`__d_reconcile/__d_stats/__d_dispose*/__d_run_effects`）。
  // 两边一旦撞名，后写的那个静默胜出——而宿主桥在本文件之前注入，
  // **本文件的同名赋值会盖掉宿主桥**，调用点随即指向本层（最容易的表现是递归）。
  // 实测踩到：整棵卸载曾经叫 `__d_unmount`，于是 `unmountVNode` 里那行
  // `__d_unmount(node.el)` 变成对「整棵卸载」的递归调用，裁子元素/摘事件全失效，
  // 表现为条件分支收不回、换页叠树、绑定计数归零（四处用例同时红）。
  // 新增本层的全局名前，先确认宿主桥里没有它。

  /// 卸载清理：切换 compose 根 / 宿主销毁时把已登记 effect 的清理都跑掉。
  /// （单根 v1：重建根时调用；子作用域级清理随 C++ 侧对齐。）
  globalThis.__d_dispose_effects = function () {
    for (const slot of hookSlots) {
      if (slot === undefined || slot.kind !== 'effect' || !slot.cleanup) continue;
      try {
        slot.cleanup();
      } catch (error) {
        log('[declarative] effect 清理异常: ' + error);
      }
      slot.cleanup = null;
    }
  };

  /// 宿主/外部请求整棵卸载（场景切换、宿主析构前）：事件反注册 + effect 清理。
  /// 返回是否真的卸了东西。
  ///
  /// 命名陷阱：本函数名与宿主窄桥同处一个全局命名空间——**不能叫 `__d_unmount`**
  /// （那是「摘单个元素」的桥，一盖就变成递归；详见上面的「桥名纪律」）。
  globalThis.__d_dispose = function () {
    if (!mounted) return false;
    if (rootVdom !== null) {
      unmountVNode(rootVdom, { created: 0, removed: 0, props: 0 });
      rootVdom = null;
    } else if (typeof __d_clear_slot === 'function') {
      __d_clear_slot();   // 子树形态：根是宿主槽位里的元素（不是我们创建的）
    }
    globalThis.__d_dispose_effects();
    hookSlots.length = 0;
    scopes.length = 0;
    eventHolders.clear();
    mounted = false;
    dirty = false;
    return true;
  };

  // ── useRef：跨重组稳定的可变槽（**改它不触发重组**）────────────────────
  function useRef(initial) {
    const slot = hookSlot('ref');
    if (!slot.init) {
      slot.init = true;
      slot.value = { current: initial };
    }
    return slot.value;
  }

  // ── usePersisted：会话级持久状态（写穿透宿主状态仓）────────────────────
  //
  // 与 `useState` 同一接口（返回 State 形态：`.value` 读写）；区别只在**初值与落盘**：
  // 值同步到宿主 `state` 对象（协议 `script.state` 可读回——AI 可观测），
  // 重启同一次会话时从那里恢复。持久化是**尽力而为**：宿主没提供 state 就退化为普通状态。
  function usePersisted(key, initial) {
    const slot = hookSlot('persisted');
    if (slot.cell === undefined) {
      // 宿主状态仓：协议 `script.state` 读回的同一个对象。
      // **必须走 `globalThis.state`**：本文件里有个同名局部函数 `state`（State 工厂），
      // 裸写 `state` 会解析到函数而不是宿主仓（实测：初值永远 fallback）。
      const store = globalThis.state;
      let start = initial;
      if (typeof store === 'object' && store !== null &&
          Object.prototype.hasOwnProperty.call(store, key)) {
        start = store[key];
      } else if (typeof store === 'object' && store !== null) {
        store[key] = initial;
      }
      const cell = state_cell(start, function (next) { store[key] = next; });   // 写穿透宿主状态仓
      slot.cell = cell;
      slot.key = key;
    }
    return slot.cell;
  }

  // ── useResource：异步状态（≈ produceState / LaunchedEffect）────────────
  //
  // 语义（与 docs/declarative.md §3.1 一致）：
  // - 返回一个 State，其值为 {status:'pending'|'ok'|'error', value?, error?}；
  // - `input` 变化 → 重发（代次计数：旧代次结果丢弃——不写旧数据进状态）；
  // - 结果落地写内部 State → 自动标脏（下一帧重组）；
  // - 依赖微任务泵：宿主每帧 `pump_jobs()`（Promises 不会自己跑）。

  function useResource(fetcher, input) {
    const slot = hookSlot('resource');
    if (slot.cell === undefined) {
      slot.token = 0;
      slot.input = undefined;
      slot.cell = state({ status: 'pending' });
    }
    const sameInput = JSON.stringify(slot.input) === JSON.stringify(input);
    if (!sameInput) {
      slot.input = input;
      const token = ++slot.token;   // 代次：旧请求回来时对不上就丢弃
      Promise.resolve()
        .then(() => fetcher(input))
        .then(function (value) {
          if (token !== slot.token) return;   // 被新输入取代：丢弃旧结果
          slot.cell.value = { status: 'ok', value: value };
        })
        .catch(function (error) {
          if (token !== slot.token) return;
          slot.cell.value = { status: 'error', error: String(error) };
        });
    }
    return slot.cell;
  }

  globalThis.useResource = useResource;
  globalThis.useMemo = useMemo;
  globalThis.useEffect = useEffect;
  globalThis.useRef = useRef;
  globalThis.usePersisted = usePersisted;

  // 宿主每帧调：dirty 才重跑（依赖 v1 全量重跑——JS 侧依赖收集已就位，
  // 但单根语义下 scope 粒度 = 根组件，细粒度跳过是 M4）
  globalThis.__d_reconcile = function () {
    if (!mounted || !dirty) return { rerun: 0 };
    const stats = { rerun: 0, created: 0, removed: 0, props: 0 };
    // 收敛循环（最多 4 轮）：
    // - 第 1 轮 = 普通重组（build → diff → 落地）；
    // - 每轮末尾跑 effect；若 effect 写了状态，dirty 会重新置真 → 再跑一轮，
    //   直到收敛（无新写入）或触上限。
    //
    // 为何要循环：`useEffect` 里写状态是常规写法（取数落地/同步派生），只跑一帧的
    // 话调一次 tick 只能推半拍——界面永远滞后一帧（实测：log= 而不是 log=1）。
    // 为何要上限：防「effect 每轮都写状态」的链子把一帧拖成死循环（未收敛的留给下一帧）。
    for (let round = 0; round < 4 && mounted && dirty; ++round) {
      dirty = false;
      hookCursor = 0;   // hooks 调用点游标：本次重组从 0 起（第 N 个 hook 调用 = 第 N 个槽）
      pendingEffects.length = 0;   // 本次 build 重新登记（旧登记作废——槽的 ran 标记保留）
      const scope = scopes[scopes.length - 1];
      enterScope(scope);
      let fresh;
      try {
        fresh = scope.build();
      } catch (error) {
        exitScope();
        log('[declarative] build 异常，冻结上一帧: ' + error);
        return { rerun: 0, error: String(error) };
      }
      exitScope();
      fresh = normalize(fresh);   // 链式修饰者：取回真 VNode
      // 首次挂载或根类型变了：整树重建；否则根节点复用 diff。
      // 子树形态：宿主槽位可能已有根（前一个 host 已挂）——采纳它进 rootVdom，
      // 让下面的「同型复用」分支接管（不重复创建）。
      if (rootVdom === null && typeof __d_slot_root === 'function') {
        const slotRoot = __d_slot_root();
        if (slotRoot) {
          rootVdom = { type: fresh.type, props: {}, key: '', children: [], el: slotRoot,
                       _applied: {} };
        }
      }
      if (rootVdom === null || rootVdom.type !== fresh.type) {
        if (rootVdom !== null) unmountVNode(rootVdom, stats);
        mountVNode(null, 0, fresh, stats);
        rootVdom = fresh;
      } else {
        fresh.el = rootVdom.el;
        const apply = propsToApply(fresh);
        const prev = rootVdom._applied || {};
        const changed = {};
        for (const key of Object.keys(apply)) {
          if (apply[key] !== prev[key]) changed[key] = apply[key];
        }
        if (Object.keys(changed).length > 0) {
          queueApply(fresh.el, changed);
          fresh._applied = apply;
        } else {
          fresh._applied = prev;
        }
        // 事件：按元素身份持有（`eventHolders`）——这里只更新间接层的指针。
        // 子树里的同型复用节点由 `reconcileAt` 各自处理。
        ensureEventBinding(fresh);
        if (isContainer(fresh)) reconcileChildren(fresh.el, rootVdom.children, fresh.children, stats);
        rootVdom = fresh;
      }
      flushApply();   // 一次跨界批量落地（新建元素挂树后也在这里补属性）
      stats.rerun = 1;
      // 副作用在重组**结束之后**执行（与 C++ 侧 reconcile 末尾同序）：
      // effect 里写状态会重新置 dirty——下面的循环条件于是再跑一轮。
      runPendingEffects();
    }
    return stats;
  };

  globalThis.__d_stats = function () {
    return { scopes: scopes.length, mounted: mounted, dirty: dirty, hooks: hookSlots.length };
  };
})();
