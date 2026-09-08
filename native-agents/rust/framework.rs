// Rust 语言基础框架（歌白多语言子代理协议 v1，见 native-agents/README.md）。
//
// 单文件零依赖（纯标准库）：语言目录 native-agents/rust/ 下共享本文件；每个子代理项目
// （语言目录下的二级目录）一个 main.rs——`#[path = "../framework.rs"] mod framework;` 引入
// 后用 framework::register_tool 注册专属工具 + main() 调用 framework::run() 即成完整边车驱动。
// 一种语言派生任意多个子代理，实现语言对模型透明（模型只看到工具与提示词）。
//
// 协议（stdin/stdout 各一行一个 JSON，UTF-8）：
//   {"id":1,"op":"init"}       → {"id":1,"ok":true,"result":{"name":"<项目目录名>","protocol":1,...}}
//   {"id":2,"op":"tools.list"} → {"id":2,"ok":true,"result":[{name,description,parameters},...]}
//   {"id":3,"op":"tool.call","args":{"tool":"b64_encode","args":{...}}}
//                              → {"id":3,"ok":true,"result":{"output":"...","data":{...}}}
// 约定：stdout 只写协议行（println! 全部禁用）；stderr 自由文本（eprintln! 排障）；
// stdin EOF → 立即退出（父进程已死，防孤儿）。
//
// 宿主注入的运行上下文（环境变量）：GEBAI_HOME（数据根）、GEBAI_AGENT_DIR（子代理项目目录）。

use std::collections::BTreeMap;
use std::io::{self, BufRead, Write};
use std::sync::Mutex;

// ---------------- 迷你 JSON ----------------

#[derive(Debug, Clone, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Arr(Vec<Json>),
    Obj(BTreeMap<String, Json>),
}

impl Json {
    pub fn str(s: impl Into<String>) -> Json {
        Json::Str(s.into())
    }

    pub fn obj(pairs: Vec<(&str, Json)>) -> Json {
        Json::Obj(pairs.into_iter().map(|(k, v)| (k.to_string(), v)).collect())
    }

    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(m) => m.get(key),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn get_str(&self, key: &str) -> Option<&str> {
        self.get(key).and_then(|j| j.as_str())
    }

    pub fn get_num(&self, key: &str) -> Option<f64> {
        match self.get(key) {
            Some(Json::Num(n)) => Some(*n),
            _ => None,
        }
    }

    /// 解析 JSON 文本（失败返回 None；够协议用：对象/数组/字符串/数值/布尔/null）。
    pub fn parse(text: &str) -> Option<Json> {
        let bytes: Vec<char> = text.chars().collect();
        let mut p = Parser { s: &bytes, i: 0 };
        p.skip_ws();
        let v = p.value()?;
        p.skip_ws();
        if p.i == p.s.len() {
            Some(v)
        } else {
            None
        }
    }

    /// 序列化（紧凑单行——协议行格式）。
    pub fn dump(&self) -> String {
        let mut out = String::new();
        self.write(&mut out);
        out
    }

    fn write(&self, out: &mut String) {
        match self {
            Json::Null => out.push_str("null"),
            Json::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
            Json::Num(n) => {
                if n.fract() == 0.0 && n.abs() < 1e15 {
                    out.push_str(&format!("{}", *n as i64));
                } else {
                    out.push_str(&format!("{}", n));
                }
            }
            Json::Str(s) => write_escaped(s, out),
            Json::Arr(a) => {
                out.push('[');
                for (i, v) in a.iter().enumerate() {
                    if i > 0 {
                        out.push(',');
                    }
                    v.write(out);
                }
                out.push(']');
            }
            Json::Obj(m) => {
                out.push('{');
                for (i, (k, v)) in m.iter().enumerate() {
                    if i > 0 {
                        out.push(',');
                    }
                    write_escaped(k, out);
                    out.push(':');
                    v.write(out);
                }
                out.push('}');
            }
        }
    }
}

fn write_escaped(s: &str, out: &mut String) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
}

struct Parser<'a> {
    s: &'a [char],
    i: usize,
}

impl<'a> Parser<'a> {
    fn skip_ws(&mut self) {
        while self.i < self.s.len() && self.s[self.i].is_whitespace() {
            self.i += 1;
        }
    }

    fn peek(&mut self) -> Option<char> {
        self.skip_ws();
        self.s.get(self.i).copied()
    }

    fn value(&mut self) -> Option<Json> {
        match self.peek()? {
            '{' => self.object(),
            '[' => self.array(),
            '"' => self.string().map(Json::Str),
            't' => self.lit("true", 4).map(|_| Json::Bool(true)),
            'f' => self.lit("false", 5).map(|_| Json::Bool(false)),
            'n' => self.lit("null", 4).map(|_| Json::Null),
            _ => self.number(),
        }
    }

    fn lit(&mut self, word: &str, len: usize) -> Option<()> {
        if self.i + len <= self.s.len() && self.s[self.i..self.i + len].iter().collect::<String>() == word {
            self.i += len;
            Some(())
        } else {
            None
        }
    }

    fn number(&mut self) -> Option<Json> {
        self.skip_ws();
        let start = self.i;
        if self.i < self.s.len() && (self.s[self.i] == '-' || self.s[self.i] == '+') {
            self.i += 1;
        }
        while self.i < self.s.len()
            && (self.s[self.i].is_ascii_digit()
                || self.s[self.i] == '.'
                || self.s[self.i] == 'e'
                || self.s[self.i] == 'E'
                || ((self.s[self.i] == '-' || self.s[self.i] == '+')
                    && (self.s[self.i - 1] == 'e' || self.s[self.i - 1] == 'E')))
        {
            self.i += 1;
        }
        if start == self.i {
            return None;
        }
        let text: String = self.s[start..self.i].iter().collect();
        text.parse::<f64>().ok().map(Json::Num)
    }

    fn string(&mut self) -> Option<String> {
        self.skip_ws();
        if self.peek()? != '"' {
            return None;
        }
        self.i += 1;
        let mut out = String::new();
        while self.i < self.s.len() {
            let c = self.s[self.i];
            self.i += 1;
            match c {
                '"' => return Some(out),
                '\\' => {
                    let e = *self.s.get(self.i)?;
                    self.i += 1;
                    match e {
                        '"' => out.push('"'),
                        '\\' => out.push('\\'),
                        '/' => out.push('/'),
                        'b' => out.push('\u{8}'),
                        'f' => out.push('\u{c}'),
                        'n' => out.push('\n'),
                        'r' => out.push('\r'),
                        't' => out.push('\t'),
                        'u' => {
                            let mut cp: u32 = 0;
                            for _ in 0..4 {
                                let h = *self.s.get(self.i)?;
                                self.i += 1;
                                cp = cp * 16 + h.to_digit(16)?;
                            }
                            // 代理对
                            if (0xD800..=0xDBFF).contains(&cp)
                                && self.s.len() > self.i + 6
                                && self.s[self.i] == '\\'
                                && self.s[self.i + 1] == 'u'
                            {
                                let save = self.i;
                                self.i += 2;
                                let mut lo: u32 = 0;
                                for _ in 0..4 {
                                    let h = *self.s.get(self.i)?;
                                    self.i += 1;
                                    lo = lo * 16 + h.to_digit(16)?;
                                }
                                if (0xDC00..=0xDFFF).contains(&lo) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                } else {
                                    self.i = save;
                                }
                            }
                            out.push(char::from_u32(cp).unwrap_or('\u{FFFD}'));
                        }
                        _ => return None,
                    }
                }
                c => out.push(c),
            }
        }
        None
    }

    fn array(&mut self) -> Option<Json> {
        self.i += 1; // [
        let mut arr = Vec::new();
        if self.peek() == Some(']') {
            self.i += 1;
            return Some(Json::Arr(arr));
        }
        loop {
            arr.push(self.value()?);
            match self.peek()? {
                ',' => {
                    self.i += 1;
                }
                ']' => {
                    self.i += 1;
                    return Some(Json::Arr(arr));
                }
                _ => return None,
            }
        }
    }

    fn object(&mut self) -> Option<Json> {
        self.i += 1; // {
        let mut map = BTreeMap::new();
        if self.peek() == Some('}') {
            self.i += 1;
            return Some(Json::Obj(map));
        }
        loop {
            let key = self.string()?;
            if self.peek()? != ':' {
                return None;
            }
            self.i += 1;
            let v = self.value()?;
            map.insert(key, v);
            match self.peek()? {
                ',' => {
                    self.i += 1;
                }
                '}' => {
                    self.i += 1;
                    return Some(Json::Obj(map));
                }
                _ => return None,
            }
        }
    }
}

// ---------------- 工具注册表 ----------------

pub struct ToolResult {
    /// 给模型看的文本。
    pub output: String,
    /// 可选结构化输出（None = 无）。
    pub data: Option<Json>,
}

#[derive(Clone)]
pub struct ToolDef {
    pub name: &'static str,
    pub description: String,
    pub parameters: Json, // JSON Schema
    pub handler: fn(&Json) -> ToolResult,
}

static REGISTRY: Mutex<Option<Vec<ToolDef>>> = Mutex::new(None);

/// 注册工具（main.rs 里启动阶段调用；同名覆盖）。panic 语义：注册仅在启动阶段（单线程），
/// 锁中毒不可能发生；协议循环开始后不再写入。
pub fn register_tool(def: ToolDef) {
    let mut guard = REGISTRY.lock().unwrap_or_else(|e| e.into_inner());
    let reg = guard.get_or_insert_with(Vec::new);
    for d in reg.iter_mut() {
        if d.name == def.name {
            *d = def;
            return;
        }
    }
    reg.push(def);
}

fn registry() -> Vec<ToolDef> {
    REGISTRY
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .as_ref()
        .cloned()
        .unwrap_or_default()
}

/// 工具 schema 快捷构造：从 JSON 文本解析（main.rs 静态数据）。
pub fn schema(text: &str) -> Json {
    Json::parse(text).unwrap_or(Json::Null)
}

/// 工具结果快捷构造。
pub fn tool_ok(output: impl Into<String>, data: Option<Json>) -> ToolResult {
    ToolResult { output: output.into(), data }
}

pub fn tool_err(output: impl Into<String>) -> ToolResult {
    ToolResult { output: output.into(), data: None }
}

// ---------------- 协议主循环 ----------------

/// 子代理项目名：{agent_dir} 目录名（宿主要求 init.name 与 manifest.name 一致；尾部空白/斜杠防御性剔除）。
pub fn agent_name() -> String {
    std::env::var("GEBAI_AGENT_DIR")
        .ok()
        .map(|s| {
            let s = s.trim_end_matches(['/', '\\', ' ', '\t']);
            s.rsplit(['/', '\\']).next().unwrap_or("rust").to_string()
        })
        .unwrap_or_else(|| "rust".to_string())
}

fn tools_list_json() -> Json {
    Json::Arr(
        registry()
        .iter()
        .map(|d| {
            Json::obj(vec![
                ("name", Json::str(d.name)),
                ("description", Json::Str(d.description.clone())),
                ("parameters", d.parameters.clone()),
            ])
        })
        .collect(),
    )
}

fn tool_call(args: &Json) -> Result<Json, String> {
    let tool = args.get_str("tool").unwrap_or("");
    let call_args = args.get("args").cloned().unwrap_or(Json::Null);
    let def = registry().into_iter().find(|d| d.name == tool);
    let Some(def) = def else {
        return Err(format!("未知工具: {}", tool));
    };
    let r = (def.handler)(&call_args);
    Ok(Json::obj(vec![("output", Json::Str(r.output)), ("data", r.data.unwrap_or(Json::Null))]))
}

fn respond(resp: &Json) -> io::Result<()> {
    let stdout = io::stdout();
    let mut lock = stdout.lock();
    lock.write_all(resp.dump().as_bytes())?;
    lock.write_all(b"\n")?;
    lock.flush()
}

/// NDJSON 主循环（stdin EOF → 返回 → main 里退出）。
pub fn run() {
    let stdin = io::stdin();
    let mut lines = stdin.lock().lines();
    while let Some(Ok(line)) = lines.next() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let resp = match Json::parse(line) {
            None => Json::obj(vec![
                ("id", Json::Null),
                ("ok", Json::Bool(false)),
                ("error", Json::str("请求解析失败")),
            ]),
            Some(req) => {
                let id = req.get_num("id");
                let op = req.get_str("op").unwrap_or("").to_string();
                let args = req.get("args").cloned().unwrap_or(Json::Null);
                let (ok, payload): (bool, Json) = if op == "init" {
                    (
                        true,
                        Json::obj(vec![
                            ("name", Json::Str(agent_name())),
                            ("protocol", Json::Num(1.0)),
                            ("lang", Json::str("rust")),
                            ("platform", Json::str(std::env::consts::OS)),
                        ]),
                    )
                } else if op == "tools.list" {
                    (true, tools_list_json())
                } else if op == "tool.call" {
                    match tool_call(&args) {
                        Ok(result) => (true, result),
                        Err(e) => (false, Json::Str(e)),
                    }
                } else {
                    (false, Json::Str(format!("未知操作: {}", op)))
                };
                Json::obj(vec![
                    ("id", id.map(Json::Num).unwrap_or(Json::Null)),
                    ("ok", Json::Bool(ok)),
                    (if ok { "result" } else { "error" }, payload),
                ])
            }
        };
        if respond(&resp).is_err() {
            break; // stdout 管道断：父进程已死
        }
    }
}

/// 便捷：main() 里直接调用（自动 std::process::exit(0)）。
pub fn main() {
    run();
    std::process::exit(0);
}
