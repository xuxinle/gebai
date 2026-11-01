#!/usr/bin/env python3
"""格斗机器人强化学习训练（PPO-clip）。

## 架构（为什么是这个形状）

```
tools/gedou_rl (C++)  --selfplay--> data.bin --+--> 本脚本 (torch PPO) --> policy.json
        ^                                       |                            |
        +------------------ --eval 胜率 <-------+                            |
                                                                             v
                                                        游戏内 `--policy policy.json` 加载
```

**战斗逻辑只有一份**（C++ 的 `Match`）。本脚本不做任何对局模拟，只读二进制经验、
算梯度、导出权重。这样"训练用的判定"与"游戏里的判定"不可能分岔。

## 二进制格式（与 `tools/gedou_rl.cpp` 的注释同源）

```
header: magic[8]="GDRL0001" | u32 obs_dim | u32 act_dim | u32 episodes | u32 steps
record: f32 obs[obs_dim] | i32 action | f32 reward | f32 pad | u8 done | u8[3] pad
```

## 做法要点

- **GAE(λ) 算优势**：按 episode（`done` 处切段）反向递推，不跨局混合。
- **PPO-clip**：比率裁剪 + 熵奖励，标准实现，不改花样。
- **棋盘式对照**：每轮同时评三个策略（随机 / 训练中的 / 手写 AI 基线），
  曲线才有意义——只报"训练中胜率"会在对手随课程变难时彻底失真。
- **可复现**：torch 与 numpy 种子固定；采集种子固定；导出权重是纯 JSON。

用法：
    py -3 tools/gedou_rl_train.py                    # 默认：12 轮，约 2-4 分钟
    py -3 tools/gedou_rl_train.py --rounds 30        # 更久
    py -3 tools/gedou_rl_train.py --curriculum       # 课程学习：易→中→难
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

import numpy as np
import torch
import torch.nn as nn

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "dev" / "bin" / ("gedou_rl.exe" if sys.platform == "win32" else "gedou_rl")
OUT_DIR = ROOT / "build" / "e2e-gedou-rl"
MAGIC = b"GDRL0001"
HEADER = 8 + 4 + 4 + 4 + 4


# ── 契约：从 C++ 读，不手抄 ─────────────────────────────────────────────────


def load_schema():
    """读 C++ 导出的契约（观测维度/动作数/奖励权重）。

    ⚠ 这是本脚本的**唯一**维度来源。手写 `obs_dim = 20` 这类常量，
    与 C++ 改动一旦不同步，就会在读数据时按错误的 stride 切分——
    得到的是"看起来正常"的乱数，训练会照常跑、只是学不会东西。
    """
    proc = subprocess.run([str(EXE), "--schema"], capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=120)
    if proc.returncode != 0:
        raise SystemExit(f"读契约失败（先 st build gedou_rl）：{proc.stderr[:400]}")
    text = proc.stdout.strip()
    start = text.find("{")
    return json.loads(text[start:])


# ── 经验文件读取 ────────────────────────────────────────────────────────────


def read_experience(path, obs_dim, act_dim):
    """读二进制经验，返回 (obs, actions, rewards, dones)。"""
    raw = Path(path).read_bytes()
    if raw[:8] != MAGIC:
        raise SystemExit(f"{path} 不是 GDRL 经验文件（magic 不符）")
    file_obs, file_act, episodes, steps = struct.unpack("<IIII", raw[8:HEADER])
    if file_obs != obs_dim or file_act != act_dim:
        raise SystemExit(f"经验维度与契约不符：文件 {file_obs}/{file_act}，契约 {obs_dim}/{act_dim}")
    record = obs_dim * 4 + 4 + 4 + 4 + 1 + 3
    expected = HEADER + record * steps
    if len(raw) < expected:
        raise SystemExit(f"经验文件被截断：期望 {expected} 字节，实得 {len(raw)}")
    # 一次性 frombuffer + reshape（逐条 unpack 会慢几十倍）
    view = np.frombuffer(raw, dtype=np.uint8, count=record * steps, offset=HEADER)
    matrix = view.reshape(steps, record)
    obs = matrix[:, : obs_dim * 4].copy().view(np.float32)
    tail = matrix[:, obs_dim * 4:]
    action = tail[:, :4].copy().view(np.int32).ravel()
    reward = tail[:, 4:8].copy().view(np.float32).ravel()
    done = tail[:, 12].astype(np.bool_)
    return obs, action, reward, done, episodes


# ── 网络 ────────────────────────────────────────────────────────────────────


class Policy(nn.Module):
    """与 C++ `PolicyNet` **逐层对应**：Linear→tanh→Linear→tanh→Linear。"""

    def __init__(self, obs_dim, act_dim, hidden=64):
        super().__init__()
        self.body = nn.Sequential(
            nn.Linear(obs_dim, hidden), nn.Tanh(),
            nn.Linear(hidden, hidden), nn.Tanh(),
            nn.Linear(hidden, act_dim),
        )
        # 小初始化：让初始策略接近均匀，早期探索面广（否则一开始就锁死在一招）。
        for layer in self.body:
            if isinstance(layer, nn.Linear):
                nn.init.orthogonal_(layer.weight, gain=0.5)
                nn.init.zeros_(layer.bias)
        self.log_std = nn.Parameter(torch.zeros(act_dim))

    def forward(self, obs):
        return self.body(obs)

    def distribution(self, obs):
        return torch.distributions.Categorical(logits=self.forward(obs))


class Critic(nn.Module):
    """**只在训练时用**的价值网络（不导出到 C++）。

    为何单独一个网络而不给 actor 加 value 头：C++ 推理端要与导出的权重
    **逐层对应**，多一个头就多一层出错面，而它只用于降低方差。
    分开之后：actor 保持"进游戏就能算"的极小形状，critic 只在训练进程里存在。
    """

    def __init__(self, obs_dim, hidden=64):
        super().__init__()
        self.body = nn.Sequential(
            nn.Linear(obs_dim, hidden), nn.Tanh(),
            nn.Linear(hidden, hidden), nn.Tanh(),
            nn.Linear(hidden, 1),
        )

    def forward(self, obs):
        return self.body(obs).squeeze(-1)


def export_policy(policy, path):
    """导出为 C++ `PolicyNet::load_json` 认识的 JSON（纯数据、行主序）。"""
    layers = []
    linears = [m for m in policy.body if isinstance(m, nn.Linear)]
    for index, layer in enumerate(linears):
        weight = layer.weight.detach().cpu().numpy()   # [out, in]
        bias = layer.bias.detach().cpu().numpy()
        layers.append({
            "in": int(weight.shape[1]),
            "out": int(weight.shape[0]),
            "tanh": index + 1 < len(linears),
            "weight": [float(v) for v in weight.reshape(-1)],
            "bias": [float(v) for v in bias],
        })
    doc = {
        "version": 1,
        "activation": "tanh",
        "observation_dim": int(linears[0].weight.shape[1]),
        "action_count": int(linears[-1].weight.shape[0]),
        "layers": layers,
    }
    Path(path).write_text(json.dumps(doc), encoding="utf-8")
    return Path(path).stat().st_size


# ── PPO ─────────────────────────────────────────────────────────────────────


def compute_gae(rewards, dones, values, gamma=0.99, lam=0.95):
    """按 episode 切段算 GAE，baseline 来自 critic。

    ⚠ 基线不能用"回报的滑动均值"：一局几百帧、局内回报高度相关，
    滑窗均值会几乎等于回报本身 ⇒ 优势退化到 0 ⇒ 损失恒为 0、熵恒为 ln8、
    胜率纹丝不动（本项目实测踩过，图上看着一切正常）。
    """
    n = len(rewards)
    advantages = np.zeros(n, dtype=np.float32)
    last = 0.0
    for index in range(n - 1, -1, -1):
        if dones[index]:
            last = 0.0
            next_value = 0.0
            mask = 0.0
        else:
            next_value = values[index + 1] if index + 1 < n else 0.0
            mask = 1.0
        delta = rewards[index] + gamma * next_value * mask - values[index]
        last = delta + gamma * lam * mask * last
        advantages[index] = last
    returns = advantages + values
    return advantages, returns


def train_epoch(policy, critic, optimizer, optimizer_critic, obs, actions, old_logprobs,
                advantages, returns, clip=0.2, entropy_coef=0.01, max_grad_norm=0.5):
    obs_t = torch.as_tensor(obs, dtype=torch.float32)
    act_t = torch.as_tensor(actions, dtype=torch.long)
    old_logp = torch.as_tensor(old_logprobs, dtype=torch.float32)
    adv_t = torch.as_tensor(advantages, dtype=torch.float32)
    ret_t = torch.as_tensor(returns, dtype=torch.float32)
    # 优势标准化：PPO 的标准做法，让步长与奖励尺度解耦。
    adv_t = (adv_t - adv_t.mean()) / (adv_t.std() + 1e-8)

    # —— critic（只训练、不导出）——
    value = critic(obs_t)
    value_loss = 0.5 * ((value - ret_t) ** 2).mean()
    optimizer_critic.zero_grad()
    value_loss.backward()
    nn.utils.clip_grad_norm_(critic.parameters(), max_grad_norm)
    optimizer_critic.step()

    # —— actor（PPO-clip）——
    dist = policy.distribution(obs_t)
    logp = dist.log_prob(act_t)
    ratio = torch.exp(logp - old_logp)
    unclipped = ratio * adv_t
    clipped = torch.clamp(ratio, 1.0 - clip, 1.0 + clip) * adv_t
    policy_loss = -torch.min(unclipped, clipped).mean()
    entropy = dist.entropy().mean()
    loss = policy_loss - entropy_coef * entropy
    optimizer.zero_grad()
    loss.backward()
    nn.utils.clip_grad_norm_(policy.parameters(), max_grad_norm)
    optimizer.step()
    return float(policy_loss.item()), float(entropy.item()), float(value_loss.item())


# ── 采集 / 评测（都交给 C++） ───────────────────────────────────────────────


def run_collect(policy_path, episodes, out_path, seed, difficulty, curriculum, eps):
    cmd = [str(EXE), "--selfplay", "--episodes", str(episodes), "--out", str(out_path),
           "--seed", str(seed), "--difficulty", difficulty, "--sample", "--epsilon", str(eps)]
    if policy_path is not None:
        cmd += ["--policy", str(policy_path)]
    if curriculum:
        cmd += ["--curriculum", str(max(1, episodes // 3))]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=1800)
    if proc.returncode != 0:
        raise SystemExit(f"采集失败：{proc.stderr[:600]}\n{proc.stdout[-600:]}")
    return proc.stdout


def run_eval(policy_path, episodes, seed, difficulty, mode="policy"):
    cmd = [str(EXE), "--eval", "--episodes", str(episodes), "--seed", str(seed),
           "--difficulty", difficulty]
    if mode == "policy" and policy_path is not None:
        cmd += ["--policy", str(policy_path)]
    elif mode == "random":
        cmd += ["--random"]
    elif mode == "script":
        cmd += ["--script-ai"]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=1800)
    if proc.returncode != 0:
        raise SystemExit(f"评测失败：{proc.stderr[:600]}")
    for line in proc.stdout.splitlines():
        if line.startswith("RESULT"):
            fields = dict(part.split("=") for part in line.split()[1:])
            return {k: float(v) for k, v in fields.items()}
    raise SystemExit(f"评测没有输出 RESULT 行：{proc.stdout[-400:]}")


def verify_forward(policy, weights_path, obs_dim, tolerance=1e-4):
    """**校验 C++ 推理 = torch 训练**（用同一批输入跑两边，逐值对比）。

    为何每次导完都要验：训练/推理是两套实现，任何一处差异（矩阵行/列、
    激活、层顺序、缺少 tanh）都会让“训练收敛、进游戏却不会打”——而曲线与
    损失看上去完全正常。本项目实测：先用这条自查定位到真因，
    否则只能在“胜率不动”这个现象上盲试。
    """
    weights = json.loads(Path(weights_path).read_text(encoding="utf-8"))
    rng = np.random.RandomState(12345)
    probes = rng.uniform(-1.0, 1.0, size=(8, obs_dim)).astype(np.float32)
    with torch.no_grad():
        expected = policy(torch.as_tensor(probes)).numpy()
    request = dict(weights)
    request["inputs"] = probes.tolist()
    proc = subprocess.run([str(EXE), "--forward-probe"], input=json.dumps(request),
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=300)
    if proc.returncode != 0:
        raise SystemExit(f"前向自检失败（进程退出 {proc.returncode}）：{proc.stderr[:400]}")
    text = proc.stdout.strip()
    start = text.find("{")
    reply = json.loads(text[start:])
    if not reply.get("ok"):
        raise SystemExit(f"前向自检失败：{reply.get('error')}")
    actual = np.array(reply["logits"], dtype=np.float32)
    if actual.shape != expected.shape:
        raise SystemExit(f"前向维度不符：C++ {actual.shape} vs torch {expected.shape}")
    diff = float(np.abs(actual - expected).max())
    if diff > tolerance:
        raise SystemExit(
            f"前向不一致（最大逐值差 {diff:.2e} > {tolerance:.0e}）——"
            "训练的网络与游戏里跑的不是同一个，先修实现再谈训练")
    return diff


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=12)
    parser.add_argument("--episodes", type=int, default=320, help="每轮采集局数")
    parser.add_argument("--epochs", type=int, default=6, help="每轮 PPO 迭代次数")
    parser.add_argument("--difficulty", default="normal")
    parser.add_argument("--curriculum", action="store_true")
    parser.add_argument("--epsilon", type=float, default=0.02)
    # 熵系数决定“探索能维持多久”。实测代价：默认 0.01 时，训练 24 轮后熵从
    # 2.08（≈均匀）降到 0.89，策略提前锁死在单招，胜率反而从 44% 掉到 14%。
    # 调大它能延长探索，但太大会一直乱打——这是本任务最敏感的一个超参。
    parser.add_argument("--entropy", type=float, default=0.05)
    # 评测局数直接决定曲线的信噪比：120 局时单局就是 0.8%，
    # 相邻两轮的“胜率差异”经常完全落在噪声里（实测：曲线在 0%~45% 之间跳）。
    # 曲线只用来定“大致趋势与最终复核”，定论靠 --eval-episodes 翻倍的复核值。
    parser.add_argument("--eval-episodes", type=int, default=200)
    parser.add_argument("--seed", type=int, default=20261101)
    parser.add_argument("--out", default=str(OUT_DIR / "policy.json"))
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if not EXE.exists():
        raise SystemExit(f"找不到 {EXE}（先 st build gedou_rl）")
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    torch.manual_seed(args.seed)
    np.random.seed(args.seed)

    schema = load_schema()
    obs_dim = int(schema["observation_dim"])
    act_dim = int(schema["action_count"])
    print(f"契约：观测 {obs_dim} 维 / 动作 {act_dim} 个 · "
          f"动作集 {[a[0] for a in schema['actions']]}")

    policy = Policy(obs_dim, act_dim)
    critic = Critic(obs_dim)
    optimizer = torch.optim.Adam(policy.parameters(), lr=3e-3)
    optimizer_critic = torch.optim.Adam(critic.parameters(), lr=3e-3)
    params = sum(p.numel() for p in policy.parameters())
    print(f"actor 参数：{params} 个（隐藏层 64×2，**进游戏的就是它**）"
          f" · critic {sum(p.numel() for p in critic.parameters())} 个（仅训练用）")

    # 基线：随机策略 + 手写 AI（同难度）。两者都只测一次——它们不随训练变化。
    random_base = run_eval(None, args.eval_episodes, args.seed, args.difficulty, mode="random")
    script_base = run_eval(None, args.eval_episodes, args.seed, args.difficulty, mode="script")
    print(f"基线：随机策略胜率 {random_base['winrate']:.1%} · "
          f"手写 AI 胜率 {script_base['winrate']:.1%}")

    history = [{
        "round": 0, "policy_winrate": None, "random_winrate": random_base["winrate"],
        "script_winrate": script_base["winrate"], "steps": 0,
    }]
    best_winrate = -1.0
    best_path = OUT_DIR / "policy-best.json"
    started = time.time()

    for round_index in range(1, args.rounds + 1):
        data_path = OUT_DIR / f"data-r{round_index:02d}.bin"
        use_policy = best_winrate > 0.0 or round_index > 1
        collected = run_collect(best_path if best_path.exists() else None,
                                args.episodes, data_path, args.seed + round_index,
                                args.difficulty, args.curriculum, args.epsilon)
        summary = [line for line in collected.splitlines() if "采集完成" in line]
        obs, actions, rewards, dones, episodes = read_experience(data_path, obs_dim, act_dim)

        # 训练前先算 old_logprobs 与 value（PPO 的比率基准与 GAE 基线，
        # 必须在**同一份权重**上算）。
        with torch.no_grad():
            obs_t = torch.as_tensor(obs, dtype=torch.float32)
            dist = policy.distribution(obs_t)
            old_logprobs = dist.log_prob(torch.as_tensor(actions, dtype=torch.long)).numpy()
            values = critic(obs_t).numpy()
        advantages, returns = compute_gae(rewards, dones, values)

        losses = []
        value_losses = []
        for _ in range(args.epochs):
            policy_loss, entropy, value_loss = train_epoch(
                policy, critic, optimizer, optimizer_critic, obs, actions, old_logprobs,
                advantages, returns, entropy_coef=args.entropy)
            losses.append(policy_loss)
            value_losses.append(value_loss)

        eval_path = OUT_DIR / "policy-iter.json"
        export_policy(policy, eval_path)
        # 每次导完都校验一次：保证“训练的这个网络”就是“游戏里跑的那个”。
        diff = verify_forward(policy, eval_path, obs_dim)
        result = run_eval(eval_path, args.eval_episodes, args.seed, args.difficulty, mode="policy")
        winrate = result["winrate"]
        improved = winrate > best_winrate
        if improved:
            best_winrate = winrate
            export_policy(policy, best_path)
        history.append({
            "round": round_index, "policy_winrate": winrate,
            "random_winrate": random_base["winrate"], "script_winrate": script_base["winrate"],
            "steps": int(len(obs)), "loss": float(np.mean(losses)), "entropy": float(entropy),
            "return": result["return"], "frames": result["frames"], "damage": result["damage"],
            "improved": bool(improved),
        })
        elapsed = time.time() - started
        print(f"[{round_index:2d}/{args.rounds}] 步数 {len(obs):6d} · 策略损失 {np.mean(losses):+.3f} "
              f"· 价值损失 {np.mean(value_losses):+.3f} · 熵 {entropy:.3f} · 胜率 {winrate:.1%} "
              f"（随机 {random_base['winrate']:.1%} / 手写 {script_base['winrate']:.1%}）"
              f"{' *新最优' if improved else ''} · {elapsed:.0f}s · 前向差 {diff:.1e}")
        data_path.unlink(missing_ok=True)

    # 最终导出：最优权重 + 最后一次权重都留（前者用于评测，后者便于继续训练）。
    export_policy(policy, args.out)
    final = run_eval(best_path, args.eval_episodes * 2, args.seed + 7, args.difficulty, mode="policy")

    report = {
        "schema": {"observation_dim": obs_dim, "action_count": act_dim},
        "baselines": {"random": random_base, "script_ai": script_base},
        "history": history,
        "best_winrate": best_winrate,
        "final_eval": final,
        "config": vars(args),
        "seconds": time.time() - started,
    }
    report_path = OUT_DIR / "train-report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    print()
    print("=== 对照（同难度对手，独立种子复核）===")
    print(f"  随机策略   胜率 {random_base['winrate']:6.1%}")
    print(f"  手写 AI    胜率 {script_base['winrate']:6.1%}")
    print(f"  训练后     胜率 {final['winrate']:6.1%}   （最佳轮 {best_winrate:.1%}）")
    print(f"  报告：{report_path}")
    print(f"  权重：{best_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
