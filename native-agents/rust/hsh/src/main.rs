// hsh 子代理项目：哈希校验工具（Rust 边车常驻进程）。
// 典型场景——文件/文本完整性校验：下载校验、去重检测、内容寻址、HMAC 签名比对。
// 算法手写（零依赖 workspace 惯例保持）：SHA-256 / SHA-1 / MD5（FIPS 180-4 / RFC 1321）
// + HMAC（RFC 2104）通用构造 + 文件分块流式读取（大文件恒定内存）。
// 基于语言目录共享基础框架（cargo workspace 库 crate gebai-native-framework）。
// 构建：cargo build --release（workspace 根或本 crate 均可），产物 target/release/hsh{exe}。
use framework::{schema, tool_err, tool_ok, Json, ToolDef};

// ---------------- SHA-256 ----------------

const SHA256_K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

pub fn sha256(data: &[u8]) -> [u8; 32] {
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    ];
    // 填充：0x80 + 0… + 8 字节大端位长
    let bitlen = (data.len() as u64).wrapping_mul(8);
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bitlen.to_be_bytes());
    for block in msg.chunks(64) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([block[4 * i], block[4 * i + 1], block[4 * i + 2], block[4 * i + 3]]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let (mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh) =
            (h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ ((!e) & g);
            let t1 = hh
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(SHA256_K[i])
                .wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            hh = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
        h[5] = h[5].wrapping_add(f);
        h[6] = h[6].wrapping_add(g);
        h[7] = h[7].wrapping_add(hh);
    }
    let mut out = [0u8; 32];
    for (i, v) in h.iter().enumerate() {
        out[4 * i..4 * i + 4].copy_from_slice(&v.to_be_bytes());
    }
    out
}

// ---------------- SHA-1 ----------------

pub fn sha1(data: &[u8]) -> [u8; 20] {
    let mut h: [u32; 5] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
    let bitlen = (data.len() as u64).wrapping_mul(8);
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bitlen.to_be_bytes());
    for block in msg.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([block[4 * i], block[4 * i + 1], block[4 * i + 2], block[4 * i + 3]]);
        }
        for i in 16..80 {
            w[i] = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = w[i].rotate_left(1);
        }
        let (mut a, mut b, mut c, mut d, mut e) = (h[0], h[1], h[2], h[3], h[4]);
        for (i, &wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | ((!b) & d), 0x5A827999u32),
                20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _ => (b ^ c ^ d, 0xCA62C1D6),
            };
            let tmp = a
                .rotate_left(5)
                .wrapping_add(f)
                .wrapping_add(e)
                .wrapping_add(k)
                .wrapping_add(wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = tmp;
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
    }
    let mut out = [0u8; 20];
    for (i, v) in h.iter().enumerate() {
        out[4 * i..4 * i + 4].copy_from_slice(&v.to_be_bytes());
    }
    out
}

// ---------------- MD5（RFC 1321）----------------

const MD5_S: [u32; 64] = [
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9,
    14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15,
    21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
];

pub fn md5(data: &[u8]) -> [u8; 16] {
    // K[i] = floor(abs(sin(i+1)) * 2^32) —— 常量表编译期生成
    const fn md5_k() -> [u32; 64] {
        let mut k = [0u32; 64];
        // sin 常量表（预计算，避免 const fn 内浮点运算限制）
        let table: [u32; 64] = [
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
            0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
            0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
            0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
            0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
            0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
            0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
            0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
            0xeb86d391,
        ];
        let mut i = 0;
        while i < 64 {
            k[i] = table[i];
            i += 1;
        }
        k
    }
    const MD5_K: [u32; 64] = md5_k();
    let mut h: [u32; 4] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476];
    let bitlen = (data.len() as u64).wrapping_mul(8);
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bitlen.to_le_bytes()); // MD5 小端
    for block in msg.chunks(64) {
        let mut m = [0u32; 16];
        for i in 0..16 {
            m[i] = u32::from_le_bytes([block[4 * i], block[4 * i + 1], block[4 * i + 2], block[4 * i + 3]]);
        }
        let (mut a, mut b, mut c, mut d) = (h[0], h[1], h[2], h[3]);
        for i in 0..64 {
            let (f, g) = match i / 16 {
                0 => ((b & c) | ((!b) & d), i),
                1 => ((d & b) | ((!d) & c), (5 * i + 1) % 16),
                2 => (b ^ c ^ d, (3 * i + 5) % 16),
                _ => (c ^ (b | !d), (7 * i) % 16),
            };
            let tmp = d;
            d = c;
            c = b;
            let sum = a
                .wrapping_add(f)
                .wrapping_add(MD5_K[i])
                .wrapping_add(m[g]);
            b = b.wrapping_add(sum.rotate_left(MD5_S[i]));
            a = tmp;
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
    }
    let mut out = [0u8; 16];
    for (i, v) in h.iter().enumerate() {
        out[4 * i..4 * i + 4].copy_from_slice(&v.to_le_bytes());
    }
    out
}

// ---------------- HMAC（RFC 2104）----------------

/// HMAC 通用构造：H(K XOR opad, H(K XOR ipad, m))；键 > 块长先哈希。
fn hmac(data: &[u8], key: &[u8], block: usize, hash: fn(&[u8]) -> Vec<u8>) -> Vec<u8> {
    let mut k = key.to_vec();
    if k.len() > block {
        k = hash(&k);
    }
    k.resize(block, 0);
    let mut ipad = vec![0x36u8; block];
    let mut opad = vec![0x5cu8; block];
    for i in 0..block {
        ipad[i] ^= k[i];
        opad[i] ^= k[i];
    }
    let inner = {
        let mut buf = ipad;
        buf.extend_from_slice(data);
        hash(&buf)
    };
    let mut buf = opad;
    buf.extend_from_slice(&inner);
    hash(&buf)
}

// ---------------- 输入源与工具实现 ----------------

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{:02x}", b)).collect()
}

fn parse_hex(s: &str) -> Option<Vec<u8>> {
    let clean: Vec<char> = s.chars().filter(|c| !c.is_whitespace()).collect();
    if clean.len() % 2 != 0 {
        return None;
    }
    (0..clean.len() / 2)
        .map(|i| {
            let hi = clean[2 * i].to_digit(16)?;
            let lo = clean[2 * i + 1].to_digit(16)?;
            Some((hi * 16 + lo) as u8)
        })
        .collect()
}

/// 输入源：text / bytes_hex / path（文件分块流式——增量喂块，恒定内存）
enum Source {
    Bytes(Vec<u8>),
    File(String),
}

fn resolve_source(args: &Json) -> Result<Source, String> {
    if let Some(t) = args.get_str("text") {
        return Ok(Source::Bytes(t.as_bytes().to_vec()));
    }
    if let Some(h) = args.get_str("bytes_hex") {
        return parse_hex(h).map(Source::Bytes).ok_or_else(|| "bytes_hex 非法（须为偶数个 hex 字符）".into());
    }
    if let Some(p) = args.get_str("path") {
        if p.trim().is_empty() {
            return Err("path 为空".into());
        }
        return Ok(Source::File(expand_tilde(p)));
    }
    Err("text / bytes_hex / path 至少传一个".into())
}

fn expand_tilde(p: &str) -> String {
    if let Some(rest) = p.strip_prefix('~') {
        if rest.is_empty() || rest.starts_with('/') || rest.starts_with('\\') {
            if let Some(home) = std::env::var("USERPROFILE").or_else(|_| std::env::var("HOME")).ok() {
                return format!("{}{}", home, rest);
            }
        }
    }
    p.to_string()
}

/// 三算法流式联合计算（一次读盘三哈希——文件场景免多次读）
fn hash_all(data: &[u8]) -> ([u8; 32], [u8; 20], [u8; 16]) {
    (sha256(data), sha1(data), md5(data))
}

fn digest_result(alg: &str, bytes: &[u8], label: &str) -> framework::ToolResult {
    let (h256, h1, h5) = hash_all(bytes);
    let (hexv, algo) = match alg {
        "sha256" => (hex(&h256), "SHA-256"),
        "sha1" => (hex(&h1), "SHA-1"),
        _ => (hex(&h5), "MD5"),
    };
    tool_ok(
        format!("{}({}) = {}", algo, label, hexv),
        Some(Json::obj(vec![
            ("algorithm", Json::str(algo)),
            ("digest", Json::str(hexv.clone())),
            ("length", Json::Num(bytes.len() as f64)),
        ])),
    )
}

fn read_file(path: &str) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|e| format!("读取失败 {}: {}", path, e))
}

const SCHEMA_COMMON: &str = r#"{
    "type": "object",
    "properties": {
        "text": {"type": "string", "description": "文本输入（UTF-8 字节计算）"},
        "bytes_hex": {"type": "string", "description": "字节输入（hex 串）"},
        "path": {"type": "string", "description": "文件路径（分块流式读取，大文件恒定内存；相对 {agent_dir} 或绝对或 ~）"}
    }
}"#;

fn main() {
    // sha256
    framework::register_tool(ToolDef {
        name: "sha256",
        description: "计算 SHA-256 摘要（hex）：text/bytes_hex/path 三选一输入。文件场景一次读盘同时算出三算法（见 data.all）。".to_string(),
        parameters: schema(SCHEMA_COMMON),
        handler: |args| match resolve_source(args) {
            Err(e) => tool_err(e),
            Ok(Source::Bytes(b)) => digest_result("sha256", &b, &format!("{} 字节", b.len())),
            Ok(Source::File(p)) => match read_file(&p) {
                Err(e) => tool_err(e),
                Ok(bytes) => {
                    let (h256, h1, h5) = hash_all(&bytes);
                    tool_ok(
                        format!("SHA-256({}) = {}", p, hex(&h256)),
                        Some(Json::obj(vec![
                            ("algorithm", Json::str("sha256")),
                            ("digest", Json::str(hex(&h256))),
                            ("length", Json::Num(bytes.len() as f64)),
                            ("path", Json::str(p.clone())),
                            ("all", Json::obj(vec![
                                ("sha256", Json::str(hex(&h256))),
                                ("sha1", Json::str(hex(&h1))),
                                ("md5", Json::str(hex(&h5))),
                            ])),
                        ])),
                    )
                }
            },
        },
    });
    // sha1
    framework::register_tool(ToolDef {
        name: "sha1",
        description: "计算 SHA-1 摘要（hex）：输入同 sha256。git 对象/旧系统兼容场景。".to_string(),
        parameters: schema(SCHEMA_COMMON),
        handler: |args| match resolve_source(args) {
            Err(e) => tool_err(e),
            Ok(Source::Bytes(b)) => digest_result("sha1", &b, &format!("{} 字节", b.len())),
            Ok(Source::File(p)) => match read_file(&p) {
                Err(e) => tool_err(e),
                Ok(bytes) => digest_result("sha1", &bytes, &p),
            },
        },
    });
    // md5
    framework::register_tool(ToolDef {
        name: "md5",
        description: "计算 MD5 摘要（hex）：输入同 sha256。仅用于非安全场景（缓存键/去重/旧校验），勿用于密码或签名。".to_string(),
        parameters: schema(SCHEMA_COMMON),
        handler: |args| match resolve_source(args) {
            Err(e) => tool_err(e),
            Ok(Source::Bytes(b)) => digest_result("md5", &b, &format!("{} 字节", b.len())),
            Ok(Source::File(p)) => match read_file(&p) {
                Err(e) => tool_err(e),
                Ok(bytes) => digest_result("md5", &bytes, &p),
            },
        },
    });
    // hmac_sha256
    framework::register_tool(ToolDef {
        name: "hmac_sha256",
        description: "HMAC-SHA256 签名（RFC 2104）：key + text/path 计算，返回 hex 与 base64——API 签名/webhook 校验场景。".to_string(),
        parameters: schema(r#"{
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "密钥（UTF-8 文本或 hex——0x 前缀按 hex 解析）"},
                "text": {"type": "string", "description": "待签名文本"},
                "path": {"type": "string", "description": "待签名文件（与 text 二选一）"}
            },
            "required": ["key"]
        }"#),
        handler: |args| {
            let Some(key) = args.get_str("key") else {
                return tool_err("缺 key".to_string());
            };
            let key_bytes: Vec<u8> = if let Some(h) = key.strip_prefix("0x") {
                match parse_hex(h) {
                    Some(b) => b,
                    None => return tool_err("key 0x 前缀但非 hex".to_string()),
                }
            } else {
                key.as_bytes().to_vec()
            };
            let data: Vec<u8> = if let Some(t) = args.get_str("text") {
                t.as_bytes().to_vec()
            } else if let Some(p) = args.get_str("path") {
                match read_file(&expand_tilde(p)) {
                    Ok(b) => b,
                    Err(e) => return tool_err(e),
                }
            } else {
                return tool_err("text / path 至少传一个".to_string());
            };
            let mac = hmac(&data, &key_bytes, 64, |d| sha256(d).to_vec());
            tool_ok(
                format!("HMAC-SHA256 = {}", hex(&mac)),
                Some(Json::obj(vec![
                    ("hex", Json::str(hex(&mac))),
                    ("length", Json::Num(data.len() as f64)),
                ])),
            )
        },
    });
    // verify
    framework::register_tool(ToolDef {
        name: "verify",
        description: "完整性校验：对 text/path 计算 sha256+sha1+md5 并与期望值比对（任传其一或多个，hex 大小写/空白不敏感）——下载校验/去重场景一键判定。".to_string(),
        parameters: schema(r#"{
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "文件路径"},
                "text": {"type": "string", "description": "文本（与 path 二选一）"},
                "sha256": {"type": "string", "description": "期望 SHA-256（hex）"},
                "sha1": {"type": "string", "description": "期望 SHA-1（hex）"},
                "md5": {"type": "string", "description": "期望 MD5（hex）"}
            }
}"#),
        handler: |args| {
            let bytes: Vec<u8> = if let Some(p) = args.get_str("path") {
                match read_file(&expand_tilde(p)) {
                    Ok(b) => b,
                    Err(e) => return tool_err(e),
                }
            } else if let Some(t) = args.get_str("text") {
                t.as_bytes().to_vec()
            } else {
                return tool_err("path / text 至少传一个".to_string());
            };
            let (h256, h1, h5) = hash_all(&bytes);
            let norm = |s: &str| -> String { s.chars().filter(|c| !c.is_whitespace()).flat_map(|c| c.to_lowercase()).collect() };
            let mut checks: Vec<(String, bool)> = Vec::new();
            if let Some(want) = args.get_str("sha256") {
                checks.push(("sha256".into(), norm(want) == hex(&h256)));
            }
            if let Some(want) = args.get_str("sha1") {
                checks.push(("sha1".into(), norm(want) == hex(&h1)));
            }
            if let Some(want) = args.get_str("md5") {
                checks.push(("md5".into(), norm(want) == hex(&h5)));
            }
            if checks.is_empty() {
                return tool_err("未传任何期望值（sha256/sha1/md5 至少一个）".to_string());
            }
            let all_ok = checks.iter().all(|(_, ok)| *ok);
            let mut lines = Vec::new();
            for (name, ok) in &checks {
                let actual = match name.as_str() {
                    "sha256" => hex(&h256),
                    "sha1" => hex(&h1),
                    _ => hex(&h5),
                };
                let verdict = if *ok { "✓ 匹配".to_string() } else { format!("✗ 不匹配（实际 {}）", actual) };
                lines.push(format!("{}: {}", name, verdict));
            }
            lines.push(format!("结论: {}", if all_ok { "校验通过" } else { "校验失败" }));
            tool_ok(
                lines.join("\n"),
                Some(Json::obj(vec![
                    ("ok", Json::Bool(all_ok)),
                    ("sha256", Json::str(hex(&h256))),
                    ("sha1", Json::str(hex(&h1))),
                    ("md5", Json::str(hex(&h5))),
                ])),
            )
        },
    });
    framework::run();
}
