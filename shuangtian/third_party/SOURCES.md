# third_party/ — 随仓库分发的第三方源码

这里的代码**已直接集成进仓库**：不是需要下载的依赖，也没有独立的版本更新流程。
构建时与框架自身源码一同编译，离线可构建、可审计。

| 依赖 | 版本 | 许可 | 用途 | 形态 |
|---|---|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | JSON 解析/序列化（替代原自研 st::json） | single-header（上游官方分发形态） |
| [quickjs-ng](https://github.com/quickjs-ng/quickjs) | 0.17.0 | MIT | 嵌入式 JS 引擎（应用内脚本层，可选） | 多文件 C 源码（仅保留引擎必需文件；已剔除 tests/CLI/wasm 与 quickjs-libc.c——后者提供 std/os 模块，本框架默认不给脚本系统访问能力） |
| [batterycenter/embed](https://github.com/batterycenter/embed) | 1.2.19 | Apache-2.0 | 编译期资源嵌入（b::embed<"path">()，含开发期热重载） | 上游为单 CMakeLists.txt；此处 vendor 其运行时与模板，构建期生成改由 stpm 原生实现（我们不依赖 CMake）——对照说明见 vendor/battery/UPSTREAM.md |

## 合规要求（保留声明与许可）

- **Apache-2.0**（batterycenter/embed）：保留 `LICENSE` 与修改声明，见 `battery/UPSTREAM.md`
- **MIT**（nlohmann/json、quickjs-ng）：保留版权与许可文本，见各自目录 `LICENSE`

## 逐文件校验和

代码已在仓库内，校验和的作用从防篡改转为**变更可追溯**：
改动第三方文件时应同步更新本表，让 review 一眼看出哪些上游代码被动了。

```bash
cd shuangtian/third_party && sha256sum -c CHECKSUMS.sha256
```

| 文件 | 字节 | SHA-256（前 16 位） |
|---|---:|---|
| `nlohmann/json.hpp` | 953436 | `aaf127c04cb31c40...` |
| `quickjs/LICENSE` | 1212 | `96f73f9d2a16c21a...` |
| `quickjs/builtin-array-fromasync.h` | 5639 | `f41eb2eb028832cf...` |
| `quickjs/builtin-iterator-zip-keyed.h` | 15993 | `24446c6157a043cf...` |
| `quickjs/builtin-iterator-zip.h` | 16213 | `fdaef760ecce82c1...` |
| `quickjs/cutils.h` | 54740 | `dffec8bea17f0810...` |
| `quickjs/dtoa.c` | 44854 | `a8d2cf2d04db406e...` |
| `quickjs/dtoa.h` | 3357 | `1c4a4540c0038632...` |
| `quickjs/libregexp-opcode.h` | 2887 | `2a98d646089f3a72...` |
| `quickjs/libregexp.c` | 115889 | `af13e996abb1767f...` |
| `quickjs/libregexp.h` | 3612 | `53ff95a038f7001b...` |
| `quickjs/libunicode-table.h` | 251190 | `5840a921ac11664b...` |
| `quickjs/libunicode.c` | 61406 | `d755606498ff707d...` |
| `quickjs/libunicode.h` | 5278 | `ad13f66aaea3fab1...` |
| `quickjs/list.h` | 3177 | `cb6e24e8ee54bccd...` |
| `quickjs/quickjs-atom.h` | 8234 | `66654ae6533d1507...` |
| `quickjs/quickjs-c-atomics.h` | 2305 | `29883c742a5c5122...` |
| `quickjs/quickjs-opcode.h` | 15831 | `0054b0c45fd091af...` |
| `quickjs/quickjs.c` | 2148068 | `9fd0e0e68856d165...` |
| `quickjs/quickjs.h` | 66974 | `747a77444ff04a91...` |
| `battery/LICENSE` | 11357 | `c71d239df91726fc...` |
| `battery/UPSTREAM.md` | 2663 | `d05f8faa374e553f...` |
| `battery/embed.hpp.in` | 4207 | `fcb297b8ed28be5f...` |
| `battery/embed_impl.cpp` | 5684 | `7b8c9c44b8e1e44e...` |
| `battery/embed_source.cpp.in` | 1394 | `a26214317ce894c4...` |

## 来源与裁剪记录

### nlohmann/json 3.12.0

- 来源：`https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp`

### quickjs-ng 0.17.0

- 来源：`https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.17.0.tar.gz`
- 归档 SHA-256：`559bc4c420475e55c7ab4510adbc562f55d7524d75e8e89d79ce4bb02f5687d9`
- 已剔除： `qjs.c`, `qjsc.c`, `ctest.c`, `api-test.c`, `lre-test.c`, `qjs-wasi*.c`, `quickjs-libc.c`, `quickjs-libc.h`, `tests/`, `examples/`

### batterycenter/embed 1.2.19

- 来源：`https://github.com/batterycenter/embed/archive/refs/tags/v1.2.19.tar.gz`
- 已修改（附声明）： `embed.hpp.in`, `embed_source.cpp.in`, `embed_impl.cpp`

