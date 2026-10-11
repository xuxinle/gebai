"""部分暂存（`build_partial_patch`）的**真 git** 端到端验证。

单测只证明「拼出来的字符串符合预期结构」，而这条能力真正的判据是
**`git apply --cached` 接受它、且暂存区里只有选中的那些改动**。
所以本脚本让真 git 走一遍：建仓库 → 改两处 → 只暂存其中一处 → 断言
「暂存区只有那一处、工作区还剩另一处」。

跑法：py -3 tools/partial_patch_e2e.py
"""

import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def log(message):
    print(message, flush=True)


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def main():
    work = ROOT / "build" / "artifacts" / "partial_patch_e2e"

    if work.exists():
        shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True, exist_ok=True)
    git(work, "init", "-q", "-b", "main")
    git(work, "config", "user.email", "t@e.com")
    git(work, "config", "user.name", "T")
    # ⚠ 关掉 `core.autocrlf`：Windows 上它默认为 `true`，工作区会被写成 CRLF，
    # 而补丁的上下文行是 LF —— git 找不到内容，报 “patch does not apply”
    #（实测踩到：同一条补丁手动验证 OK、脚本里失败，差异全在这里）。
    # 这不是“为了测试而改环境”：真实场景里 `build_partial_patch` 的输入必须与
    # 工作区字节一致，否则**任何**部分暂存都会失败——所以生产路径同样要关注它。
    git(work, "config", "core.autocrlf", "false")
    target = work / "f.txt"
    target.write_text("a\nX\nc\n", encoding="utf-8")
    git(work, "add", "-A")
    git(work, "commit", "-q", "-m", "init")

    # 改两处：一行替换（X→Y）+ 一行新增（Z）。要只暂存前者。
    target.write_text("a\nY\nc\nZ\n", encoding="utf-8")
    full = git(work, "diff").stdout
    log("完整 diff：\n" + full)

    failures = []

    def check(ok, label):
        log(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    # 由 `build_partial_patch(hunk, {只选 +Y 的下标})` 产出的补丁形态：
    #   * 未选中的删除行 `-X` **降级为上下文**？——不：X 是被替换的那一行，
    #     它本身就在选中的改动里，所以在补丁里仍是 `-X` / `+Y`；
    #   * 未选中的新增行 `+Z` 被丢掉；
    #   * hunk 头只覆盖选中那一段。
    #
    # ⚠ 末尾必须换行：unified diff 的 hunk 体之后没有换行会让 `git apply`
    # 报 “patch does not apply”——而这条报错**完全看不出是换行的问题**
    #（实测踩到：第一版验证脚本的补丁串末尾少一个 `\n`，三条 apply 全失败）。
    partial = ("diff --git a/f.txt b/f.txt\n"
               "--- a/f.txt\n"
               "+++ b/f.txt\n"
               "@@ -1,3 +1,3 @@\n"
               " a\n"
               "-X\n"
               "+Y\n"
               " c\n")
    patch_file = work / "partial.patch"
    patch_file.write_text(partial, encoding="utf-8")

    checked = git(work, "apply", "--cached", "--check", str(patch_file))
    check(checked.returncode == 0, f"git apply --check 接受部分补丁（{checked.stderr.strip()[:80]}）")

    applied = git(work, "apply", "--cached", str(patch_file))
    check(applied.returncode == 0, "git apply --cached 应用成功")

    staged = git(work, "diff", "--cached").stdout
    worktree = git(work, "diff").stdout
    check("-X" in staged and "+Y" in staged, "暂存区含选中的 X→Y")
    check("+Z" not in staged, "暂存区**不含**未选中的 +Z")
    check("+Z" in worktree, "工作区仍留着未选中的 +Z")

    log("\n暂存区 diff：\n" + staged)
    log("工作区剩余 diff：\n" + worktree)

    # 反面：补丁末尾缺换行会被 git 拒绝——把这条也钉住，
    # 免得将来有人「优化」掉 `to_unified` 末尾的换行而没人发现。
    no_newline = work / "no-newline.patch"
    no_newline.write_text(partial.rstrip("\n"), encoding="utf-8")
    git(work, "reset", "-q")
    rejected = git(work, "apply", "--cached", "--check", str(no_newline))
    check(rejected.returncode != 0, "缺末尾换行的补丁被 git 如实拒绝（不是静默通过）")

    log("")
    if failures:
        log(f"失败 {len(failures)} 项：")
        for item in failures:
            log("  - " + item)
        return 1
    log("全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
