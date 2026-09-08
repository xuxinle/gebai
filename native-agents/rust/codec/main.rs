// codec 子代理项目：高性能编解码工具（Rust 实现，边车常驻进程）。
// 基于语言目录共享基础框架（native-agents/rust/framework.rs），本文件只写工具逻辑——
// 框架处理协议（init/tools.list/tool.call + NDJSON 行循环）。
#![allow(static_mut_refs)]

#[path = "../framework.rs"]
mod framework;

use framework::{schema, tool_err, tool_ok, Json, ToolDef};

// ---------------- base64 ----------------

const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

fn b64_encode(data: &[u8]) -> String {
    let mut out = String::with_capacity((data.len() + 2) / 3 * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = *chunk.get(1).unwrap_or(&0) as u32;
        let b2 = *chunk.get(2).unwrap_or(&0) as u32;
        let n = (b0 << 16) | (b1 << 8) | b2;
        out.push(B64[(n >> 18) as usize & 63] as char);
        out.push(B64[(n >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 { B64[(n >> 6) as usize & 63] as char } else { '=' });
        out.push(if chunk.len() > 2 { B64[n as usize & 63] as char } else { '=' });
    }
    out
}

fn b64_decode(text: &str) -> Result<Vec<u8>, String> {
    let mut table = [255u8; 256];
    for (i, c) in B64.iter().enumerate() {
        table[*c as usize] = i as u8;
    }
    let clean: Vec<u8> = text.bytes().filter(|b| !b.is_ascii_whitespace()).collect();
    if clean.len() % 4 != 0 {
        return Err(format!("长度 {:?} 非 4 的倍数（base64 应为 4n 字符）", clean.len()));
    }
    let mut out = Vec::with_capacity(clean.len() / 4 * 3);
    for chunk in clean.chunks(4) {
        let mut n: u32 = 0;
        let mut pad = 0;
        for (i, &c) in chunk.iter().enumerate() {
            if c == b'=' {
                // = 只允许出现在末尾 1-2 位
                if i < 2 || chunk[i + 1..].iter().any(|&x| x != b'=') {
                    return Err("非法填充位置".to_string());
                }
                pad += 1;
                n <<= 6;
            } else {
                let v = table[c as usize];
                if v == 255 {
                    return Err(format!("非法字符 '{}'", c as char));
                }
                n = (n << 6) | v as u32;
            }
        }
        out.push((n >> 16) as u8);
        if pad < 2 {
            out.push((n >> 8) as u8);
        }
        if pad < 1 {
            out.push(n as u8);
        }
    }
    Ok(out)
}

// ---------------- crc32 ----------------

fn crc32_table() -> [u32; 256] {
    let mut table = [0u32; 256];
    for (i, slot) in table.iter_mut().enumerate() {
        let mut c = i as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB88320 ^ (c >> 1) } else { c >> 1 };
        }
        *slot = c;
    }
    table
}

fn crc32(data: &[u8]) -> u32 {
    let table = crc32_table();
    let mut c: u32 = 0xFFFF_FFFF;
    for &b in data {
        c = table[((c ^ b as u32) & 0xFF) as usize] ^ (c >> 8);
    }
    c ^ 0xFFFF_FFFF
}

// ---------------- hex ----------------

fn hex_encode(data: &[u8]) -> String {
    let mut out = String::with_capacity(data.len() * 2);
    for b in data {
        out.push_str(&format!("{:02x}", b));
    }
    out
}

fn hex_decode(text: &str) -> Result<Vec<u8>, String> {
    let clean: Vec<char> = text.chars().filter(|c| !c.is_whitespace()).collect();
    if clean.len() % 2 != 0 {
        return Err("hex 长度须为偶数".to_string());
    }
    let mut out = Vec::with_capacity(clean.len() / 2);
    for pair in clean.chunks(2) {
        let hi = pair[0].to_digit(16).ok_or_else(|| format!("非法字符 '{}'", pair[0]))?;
        let lo = pair[1].to_digit(16).ok_or_else(|| format!("非法字符 '{}'", pair[1]))?;
        out.push((hi * 16 + lo) as u8);
    }
    Ok(out)
}

// ---------------- 工具注册 ----------------

fn register_all() {
    framework::register_tool(ToolDef {
        name: "b64_encode",
        description: "文本或字节按 base64 编码（UTF-8 文本或 hex 字节串输入；常驻进程零启动开销）。".to_string(),
        parameters: schema(
            r#"{
                "type": "object",
                "properties": {
                    "text": {"type": "string", "description": "待编码文本（UTF-8）"},
                    "bytes_hex": {"type": "string", "description": "待编码字节（hex 串，与 text 二选一）"}
                }
            }"#,
        ),
        handler: |args| {
            let data: Vec<u8> = if let Some(hex) = args.get_str("bytes_hex") {
                match hex_decode(hex) {
                    Ok(d) => d,
                    Err(e) => return tool_err(format!("bytes_hex 解析失败: {}", e)),
                }
            } else if let Some(t) = args.get_str("text") {
                t.as_bytes().to_vec()
            } else {
                return tool_err("text 与 bytes_hex 至少传一个".to_string());
            };
            let encoded = b64_encode(&data);
            tool_ok(
                encoded.clone(),
                Some(Json::obj(vec![
                    ("encoded", Json::str(encoded)),
                    ("inputBytes", Json::Num(data.len() as f64)),
                ])),
            )
        },
    });

    framework::register_tool(ToolDef {
        name: "b64_decode",
        description: "base64 解码：默认输出 UTF-8 文本；binary=true 输出 hex 字节串（不可打印数据用）。".to_string(),
        parameters: schema(
            r#"{
                "type": "object",
                "properties": {
                    "text": {"type": "string", "description": "base64 文本（容忍空白字符）"},
                    "binary": {"type": "boolean", "description": "true 输出 hex；缺省尝试 UTF-8 文本"}
                },
                "required": ["text"]
            }"#,
        ),
        handler: |args| {
            let Some(text) = args.get_str("text") else {
                return tool_err("缺 text".to_string());
            };
            let binary = matches!(args.get("binary"), Some(Json::Bool(true)));
            match b64_decode(text) {
                Ok(bytes) => {
                    if binary {
                        let hex = hex_encode(&bytes);
                        tool_ok(hex.clone(), Some(Json::obj(vec![("bytesHex", Json::str(hex)), ("length", Json::Num(bytes.len() as f64))])))
                    } else {
                        match String::from_utf8(bytes.clone()) {
                            Ok(s) => tool_ok(s.clone(), Some(Json::obj(vec![("text", Json::str(s)), ("length", Json::Num(bytes.len() as f64))]))),
                            Err(_) => tool_err("解码结果非 UTF-8——传 binary=true 取 hex".to_string()),
                        }
                    }
                }
                Err(e) => tool_err(format!("解码失败: {}", e)),
            }
        },
    });

    framework::register_tool(ToolDef {
        name: "crc32",
        description: "计算文本/字节串的 CRC-32（IEEE 802.3，与 zlib/png 一致），返回 8 位十六进制。".to_string(),
        parameters: schema(
            r#"{
                "type": "object",
                "properties": {
                    "text": {"type": "string", "description": "文本（UTF-8 字节计算）"},
                    "bytes_hex": {"type": "string", "description": "字节串（hex，与 text 二选一）"}
                }
            }"#,
        ),
        handler: |args| {
            let data: Vec<u8> = if let Some(hex) = args.get_str("bytes_hex") {
                match hex_decode(hex) {
                    Ok(d) => d,
                    Err(e) => return tool_err(format!("bytes_hex 解析失败: {}", e)),
                }
            } else if let Some(t) = args.get_str("text") {
                t.as_bytes().to_vec()
            } else {
                return tool_err("text 与 bytes_hex 至少传一个".to_string());
            };
            let c = crc32(&data);
            tool_ok(
                format!("{:08x}", c),
                Some(Json::obj(vec![
                    ("crc32", Json::str(format!("{:08x}", c))),
                    ("decimal", Json::Num(c as f64)),
                    ("length", Json::Num(data.len() as f64)),
                ])),
            )
        },
    });
}

fn main() {
    register_all();
    framework::run();
}
