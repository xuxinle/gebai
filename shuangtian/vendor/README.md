# vendor/ — 第三方源码（vended third-party）

本目录存放**外部源码**。框架自身代码在 `include/st/`、`src/`，与此处严格分开——
这里的每个文件都来自上游，改动即失去"可追溯"的意义（要改就升级版本，不要就地修补）。

## 台账

| 依赖 | 版本 | 许可 | 用途 | 形态 |
|---|---|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | JSON 解析/序列化 | 官方单头 `json.hpp` |
| [quickjs-ng](https://github.com/quickjs-ng/quickjs) | 0.17.0 | MIT | 嵌入式 JS 引擎（应用内脚本层） | 多文件 C 源码（精简） |

- **`sources.json`**：权威台账——版本、来源 URL、许可、每个文件的 SHA-256、以及 QuickJS 的**剔除清单**
  （为什么少文件是可审计的）。
- **`CHECKSUMS.sha256`**：机器可校验清单，`sha256sum -c` 直接可用。

## 校验

```bash
cd shuangtian/vendor
sha256sum -c CHECKSUMS.sha256      # 全部 OK = 与台账一致（未被就地修改）
```

## QuickJS 为什么只有 19 个文件

上游源码树还包含 CLI（`qjs`/`qjsc`）、测试（`ctest`/`api-test`/`lre-test`）、WASM 变体，
以及 `quickjs-libc.c`。本框架只保留**引擎必需**的部分：

- 必需：`quickjs.c`（引擎）、`libregexp.c`（正则）、`libunicode.c` + `libunicode-table.h`（Unicode）、
  `dtoa.c`（数字↔字符串）与它们引用的头文件；
- **刻意剔除 `quickjs-libc.c`**：它提供 `std`/`os` 模块（文件、进程、socket）——
  本框架的脚本层只服务于界面逻辑，**默认不给脚本系统访问能力**。
  若将来需要，再单独引入并同步台账。

剔除清单记录在 `sources.json` 的 `excluded` 字段。

## 升级流程

1. 取上游新版本（URL 见 `sources.json`）；
2. 替换 `vendor/<name>/` 下文件，更新 `sources.json` 的 `version`/`url`/`archive_sha256`/`files`；
3. 重新生成 `CHECKSUMS.sha256`；
4. 跑 `st test`（含 `--san`）与 `st lint`；QuickJS 升级后**必须**跑脚本引擎用例；
5. 在提交信息里写明版本变更与验证结论。

> 台账与校验和的存在意义：让"我们到底依赖了什么、版本是多少、有没有被就地改过"这三个问题
> 有一份**可机检的答案**——而不是靠记忆与注释。
