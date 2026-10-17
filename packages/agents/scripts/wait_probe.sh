#!/usr/bin/env bash
# wait_probe.sh —— 「等待纪律」的可执行验收：`pgrep -f` 自匹配活锁的**六项对照**
#
# ## 为什么把它入仓（原先在 /tmp，重启即丢 ⇒ 引用悬空）
#
# `packages/agents/src/agents/shengsheng/pgrep-lock.ts` 的头注释、`shengsheng.md` 的「等待纪律」、
# `shuangtian/docs/PITFALLS.md` 的「`pgrep -f <pattern>` 会匹配到发起它的那条 shell」都引用了
# 那批实测数字（错法 5 次等待 → 5 次判「还在跑」；破字串后 → 0 次）。证据脚本若丢，这些数字
# 就退化成「讲故事」。本脚本入仓后，那批数字变成**可复跑**的断言。
#
# ## 六项对照（每一项都可判真假，不是打印完就算）
#
#   ① 场上真实进程数   —— 破字判据数到的真实 gate 数（期望 1；证明破字串**语义未变**，照样命中真目标）
#   ② 错法判据         —— pattern 原样写在等待命令行里：gate 已死，5 次等待**5 次**仍判「还在跑」
#   ③ 匹配到的是自己   —— 错法命中的 pid **就是发起它的那条 shell 自己**（这就是全部成因）
#   ④ `kill -0 <pid>`  —— 有 pid 就别用 pgrep：活着=LIVE / 收尸后=GONE（从不自匹配）
#   ⑤ `pgrep -x <名>`  —— 只比 comm、**不扫命令行**：按名能找到目标，而命令行里带标记的 shell 扫不到
#   ⑥ 破字串           —— 同一个等待形态换成 `x[.]py`：gate 已死时 **0 次**判「还在跑」
#
# ## 用法
#
#   bash packages/agents/scripts/wait_probe.sh          # 正向：三种对法均符合预期 ⇒ exit 0
#   bash packages/agents/scripts/wait_probe.sh --naive  # 反向：把「破字串」改回原样串 ⇒ **必须报红**
#
# `--naive` 不是打个标志位：它把本文件里 `BROKEN=` 那一行用 `sed` 改写成原样串、把改写结果写到
# 临时副本里再跑一遍（并断言改写真的生效）。因此它验的是**真源码行**，不是另一条代码路径——
# 这正是「证明它真能抓住自匹配」的要求。
#
# ## 纪律（本脚本自己必须守，否则它就是下一个自匹配现场）
#
# · 只用 `mktemp -d` 建临时目录，**不碰主树**（对仓里任何文件都只读）；
# · 标记串**运行时随机生成** ⇒ 脚本文本里不含被测串原文；
# · 起的进程全部 trap 清掉；gate 用「短 sleep 循环」而非「一次长 sleep」，收尸不留孤儿；
# · 等待一律带**上限**（`for i in $(seq 1 N)`，N 有限）。
#
set -uo pipefail

# ── 反向：把破字串改回原样串，在临时副本里重跑（验真源码行，不是验标志位）──────────────
if [ "${1:-}" = "--naive" ] && [ "${WAIT_PROBE_INNER:-}" != "1" ]; then
  SELF="${BASH_SOURCE[0]}"
  NAIVE_TMP="$(mktemp -d "${TMPDIR:-/tmp}/wait_probe_naive.XXXXXX")" || exit 2
  NAIVE_SH="$NAIVE_TMP/wait_probe_naive.sh"
  # 只改这一行：字符类断开 → 原样串
  sed 's|^  BROKEN="\${MARK%\.py}\[\.\]py".*$|  BROKEN="$MARK"   # 反向：原样串（改成这样必须报红）|' "$SELF" >"$NAIVE_SH"
  if ! grep -qxF '  BROKEN="$MARK"   # 反向：原样串（改成这样必须报红）' "$NAIVE_SH"; then
    echo "✗ 反向改写没生效（本脚本的 BROKEN= 行变了？）——反向验证本身作废" >&2
    rm -rf "$NAIVE_TMP"; exit 2
  fi
  echo "== 反向验证：把破字串改回原样串（临时副本：$NAIVE_SH）=="
  WAIT_PROBE_INNER=1 bash "$NAIVE_SH"
  RC=$?
  rm -rf "$NAIVE_TMP"
  echo
  if [ "$RC" -ne 0 ]; then
    echo "✓ 反向验证通过：改回原样串后脚本报红（exit=$RC）——它抓的就是自匹配本身"
    exit 0
  fi
  echo "✗ 反向验证失败：改回原样串后脚本竟然全绿（exit=0）——六项对照没有一项能抓住自匹配" >&2
  exit 1
fi

if ! command -v pgrep >/dev/null 2>&1; then
  echo "✗ 没有 pgrep：本脚本验的就是 pgrep 的语义，跑不了" >&2
  exit 2
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/wait_probe.XXXXXX")" || { echo "✗ mktemp 失败" >&2; exit 2; }
GATE_PID=""
NAMED_PID=""

cleanup() {
  [ -n "$GATE_PID" ] && kill -TERM "$GATE_PID" 2>/dev/null
  [ -n "$NAMED_PID" ] && kill -TERM "$NAMED_PID" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT

# 数进程：`pgrep -c` 无命中时也会打印 0 且退出 1（`|| echo 0` 会变成两行），故统一走 wc
count_f() { pgrep -f "$1" 2>/dev/null | wc -l | tr -d ' '; }
count_x() { pgrep -x "$1" 2>/dev/null | wc -l | tr -d ' '; }

# ── 被测标记：运行时随机 ⇒ 脚本文本里没有被测串的原文 ────────────────────────────────
MARK="sgs_gate_probe_$$_${RANDOM}.py"
  BROKEN="${MARK%.py}[.]py"      # 正向：字符类断开最后一个字面字符（正则语义不变）
# （上一行是 `--naive` 的改写目标——改行名/缩进时请同步脚本里的 sed）

FAILS=0
ok()   { printf '  ✓ %s\n' "$*"; }
bad()  { printf '  ✗ %s\n' "$*"; FAILS=$((FAILS+1)); }
check(){ [ "$1" = "$2" ] && ok "$3（实测 $1）" || bad "$3（实测 $1，期望 $2）"; }

printf '== wait_probe：pgrep -f 自匹配活锁六项对照 ==\n'
printf '标记：%s\n判据：%s\n\n' "$MARK" "$BROKEN"

# ── 起一个「真 gate」：命令行里带标记、存活到被收掉 ──────────────────────────────────
# 用「短 sleep 循环」而不是「一次长 sleep / 脚本里嵌 sleep 120」：后者的子进程是孤儿，
# 收掉父 shell 后它还在（实测漏过一个 `sleep 120`），既脏场又会让 ① 数错。
# 循环体里的 sleep 只活 0.5s，且其命令行**不含标记** ⇒ 不会被数进 ①（不会重复计数）。
bash -c 'while :; do sleep 0.5; done' "$MARK" >/dev/null 2>&1 &
GATE_PID=$!
for i in $(seq 1 50); do [ "$(count_f "$BROKEN")" -gt 0 ] && break; sleep 0.1; done

# ── ① 场上真实进程数（破字判据照样命中真目标 ⇒ 破字不改变语义）──────────────────────
REAL=$(count_f "$BROKEN")
check "$REAL" 1 "① 场上真实进程数（破字判据）"
if pgrep -f "$BROKEN" 2>/dev/null | grep -qx "$GATE_PID"; then
  ok "① 破字判据命中的正是那个 gate（pid=$GATE_PID）"
else
  bad "① 破字判据没命中 gate（pid=$GATE_PID）——破字串把语义也一起改坏了？"
fi

# ── 收掉 gate（gate 就是那个 bash 循环本体，kill 它即可，不留长命孤儿）────────────────
kill -TERM "$GATE_PID" 2>/dev/null
wait "$GATE_PID" 2>/dev/null
GATE_PID=""
for i in $(seq 1 50); do [ "$(count_f "$BROKEN")" -eq 0 ] && break; sleep 0.1; done

# ── ② 错法判据：pattern 原样写在自己的等待命令行里 ─────────────────────────────────
# 形态照抄现场：`sh -c "for i in …; do pgrep -f '<串>' …; done"` —— 这条 shell 自己的命令行就着串。
# 串用 argv 传而不是拼进脚本体：`pgrep -f` 匹配的是 /proc/pid/cmdline，**argv 同样算**（所以照样自匹配），
# 而这样不必和引号搏斗。
WRONG_N=$(bash -c 'n=0; for i in 1 2 3 4 5; do if pgrep -f "$1" >/dev/null 2>&1; then n=$((n+1)); fi; done; echo "$n"' _ "$MARK")
check "$WRONG_N" 5 "② 错法（原样串）：gate 已死，5 次等待里判「还在跑」的次数"
[ "$WRONG_N" -gt 0 ] && ok "② 条件恒真 ⇒ 这就是「等一个早已结束的东西」"

# ── ③ 终究匹配到了谁：发起它的那条 shell 自己 ───────────────────────────────────
MATCH_OUT=$(bash -c 'echo "SELF=$$"; echo "CMDLINE=$(tr "\0" " " < /proc/$$/cmdline 2>/dev/null)"; pgrep -f "$1" 2>/dev/null | sed "s/^/MATCH=/"' _ "$MARK")
SELF_PID=$(printf '%s\n' "$MATCH_OUT" | sed -n 's/^SELF=//p')
SELF_CMD=$(printf '%s\n' "$MATCH_OUT" | sed -n 's/^CMDLINE=//p' | cut -c1-110)
MATCHED=$(printf '%s\n' "$MATCH_OUT" | sed -n 's/^MATCH=//p' | tr '\n' ' ')
if printf '%s\n' "$MATCHED" | tr ' ' '\n' | grep -qx "$SELF_PID"; then
  ok "③ 错法命中的就是它自己（self=$SELF_PID，matched=[$MATCHED]）"
  [ -n "$SELF_CMD" ] && printf '     被命中的命令行：%s\n' "$SELF_CMD"
else
  bad "③ 错法没命中自己（self=$SELF_PID，matched=[$MATCHED]）——现场形态变了？"
fi

# ── ④ 对法1：kill -0 <pid>（有 pid 就别用 pgrep）────────────────────────────────
sleep 0.5 &
SHORT_PID=$!
S1=GONE; kill -0 "$SHORT_PID" 2>/dev/null && S1=LIVE
# ⚠ 必须 `wait` 收尸再判：僵尸进程对 `kill -0` 仍返回 0（未被回收的 pid 还「存在」），
#   不收尸就会量出一个假的 LIVE——那会把「对法1 有效」这条结论直接推翻。
wait "$SHORT_PID" 2>/dev/null
S2=LIVE; kill -0 "$SHORT_PID" 2>/dev/null || S2=GONE
check "$S1" LIVE "④ 对法1 kill -0 <pid>：进程跑着时"
check "$S2" GONE "④ 对法1 kill -0 <pid>：收尸结束后"
ok "④ 对法1 的判据是 pid 本身，命令行里没有任何 pattern ⇒ 自匹配无从发生"

# ── ⑤ 对法2：pgrep -x 只比进程名（comm），不扫命令行 ──────────────────────────────
# 目标进程用「一份 bash 副本 + 改名」：comm 直接就是那个名字。
# ⚠ 不能用 `cp /bin/sleep`：本机 coreutils 是多调用二进制，改名后 argv[0] 认不出来，
#   进程立刻报 `unknown program` 退出（实测：改名 sleep 起不来，-x 永远数到 0）。
cp /bin/bash "$TMP/sgsprobe" 2>/dev/null && {
  "$TMP/sgsprobe" -c 'while :; do sleep 0.5; done' >/dev/null 2>&1 &
  NAMED_PID=$!
}
for i in $(seq 1 50); do [ "$(count_x sgsprobe)" -gt 0 ] && break; sleep 0.1; done
check "$(count_x sgsprobe)" 1 "⑤ 对法2 pgrep -x sgsprobe（按进程名找到目标）"
# 关键一条：命令行里带标记的 shell，`-x <标记>` 扫不到它 ⇒ -x 无从自匹配
XHIT=$(bash -c 'pgrep -x "$1" 2>/dev/null | wc -l' _ "$MARK" | tr -d ' ')
check "$XHIT" 0 "⑤ pgrep -x <标记串> 命中数（-x 不扫命令行 ⇒ 扫不到自己）"

# ── ⑥ 对法3：破字串（同一等待形态，gate 已死）────────────────────────────────────
RIGHT_N=$(bash -c 'n=0; for i in 1 2 3; do if pgrep -f "$1" >/dev/null 2>&1; then n=$((n+1)); fi; done; echo "$n"' _ "$BROKEN")
check "$RIGHT_N" 0 "⑥ 对法3 破字串：3 次等待里判「还在跑」的次数"

# ── 结论 ───────────────────────────────────────────────────────────────────────
printf '\n'
if [ "$FAILS" -eq 0 ]; then
  echo "✓ 三种对法均符合预期（kill -0 / pgrep -x / 破字串）；错法现场已在 ②③ 复现"
  exit 0
fi
echo "✗ 有 $FAILS 项不符合预期" >&2
exit 1
