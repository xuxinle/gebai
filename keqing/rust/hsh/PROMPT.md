# hsh —— 哈希校验（Rust）

文件/文本完整性校验子代理：SHA-256 / SHA-1 / MD5 摘要 + HMAC-SHA256 签名 + 多算法联合校验。

## 工具

- `hsh_sha256(text?|bytes_hex?|path?)`：SHA-256 摘要（hex）。文件输入时 data 附三算法结果（一次读盘全算）。
- `hsh_sha1(...)`：SHA-1 摘要——git 对象、旧系统兼容。
- `hsh_md5(...)`：MD5 摘要——仅限缓存键/去重等非安全场景。
- `hsh_hmac_sha256(key, text?|path?)`：HMAC-SHA256（RFC 2104）——API 签名、webhook 校验。
- `hsh_verify(path?|text?, sha256?, sha1?, md5?)`：计算三算法并与期望值比对（大小写/空白不敏感），一键判定校验通过/失败。

## 算法与实现

- 三算法全部手写（FIPS 180-4 / RFC 1321），零第三方依赖（workspace 惯例）
- HMAC 通用构造（RFC 2104）：键超块长先哈希、ipad/opad 填充
- 算法正确性锚点：空串 SHA-256=e3b0c442…、SHA-1=da39a3ee…、MD5=d41d8cd9…；"abc" 三算法可对RFC 测试向量自证

## 典型用法

1. 下载校验：`hsh_verify {"path": "dist/app.zip", "sha256": "发布方给的值"}` → ✓/✗ 一键判定
2. 内容寻址/去重：`hsh_sha256 {"path": "blob.bin"}` → digest 作键
3. API 签名：`hsh_hmac_sha256 {"key": "secret", "text": "待签名串"}` → hex
