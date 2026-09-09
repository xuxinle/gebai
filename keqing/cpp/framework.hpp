// C++ 语言基础框架（歌白客卿协议 v1，见 keqing/README.md）。
//
// 头文件式框架：语言目录 keqing/cpp/ 下共享本文件与 build 脚本；每个子代理项目
// （语言目录下的二级目录）一个 main.cpp——`#include "../framework.hpp"` 后用 REGISTER_TOOL
// 注册专属工具 + main() 里调用 gebai::run() 即成完整边车驱动。一种语言派生任意多个
// 子代理，实现语言对模型透明（模型只看到工具与提示词）。
//
// 零第三方依赖（C++17，纯标准库）：手写迷你 JSON 解析/序列化（够协议用：对象/数组/字符串/
// 数值/布尔/null，\uXXXX 转义解码），NDJSON 行循环。
//
// 协议（stdin/stdout 各一行一个 JSON，UTF-8）：
//   {"id":1,"op":"init"}       → {"id":1,"ok":true,"result":{"name":"<项目目录名>","protocol":2,...}}
//   {"id":2,"op":"tools.list"} → {"id":2,"ok":true,"result":[{name,description,parameters},...]}
//   {"id":3,"op":"tool.call","tool":"eval","args":{...},"ctx":{sessionId,user,cwd,env,sandboxed}}
//                              → {"id":3,"ok":true,"result":{"output":"...","data":{...}}}
// 约定：stdout 只写协议行（printf/cout 全部禁用或重定向）；stderr 自由文本（宿主环形缓冲排障）；
// stdin EOF → 立即退出（父进程已死，防孤儿）。
//
// 宿主注入的运行上下文：环境变量 GEBAI_HOME（数据根）/GEBAI_AGENT_DIR（子代理项目目录，驱动
// 自身资产定位）；**请求级 ctx 随每次 tool.call 传递**（边车为进程单例跨会话共享，会话 cwd/env/
// 标识只能随请求走——用 ctx()/ctxEnv()/ctxResolve() 读取，禁止写环境变量：并发请求不同会话互踩）。
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#if defined(_WIN32)
#include <direct.h>
#endif
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gebai {

// ---------------- 迷你 JSON ----------------

struct Json {
    enum class Type { Null, Bool, Num, Str, Arr, Obj };
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj; // 保序键值对

    Json() = default;
    static Json makeBool(bool v) { Json j; j.type = Type::Bool; j.b = v; return j; }
    static Json makeNum(double v) { Json j; j.type = Type::Num; j.num = v; return j; }
    static Json makeStr(std::string v) { Json j; j.type = Type::Str; j.str = std::move(v); return j; }
    static Json makeArr() { Json j; j.type = Type::Arr; return j; }
    static Json makeObj() { Json j; j.type = Type::Obj; return j; }

    bool isNull() const { return type == Type::Null; }
    bool isStr() const { return type == Type::Str; }
    bool isNum() const { return type == Type::Num; }
    bool isObj() const { return type == Type::Obj; }
    bool isArr() const { return type == Type::Arr; }
    bool isBool() const { return type == Type::Bool; }

    // 对象取值（无该键返回 null）
    const Json* get(const std::string& key) const {
        if (type != Type::Obj) return nullptr;
        for (auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }
    void set(const std::string& key, Json v) {
        for (auto& [k, _] : obj)
            if (k == key) { _ = std::move(v); return; }
        obj.emplace_back(key, std::move(v));
    }
    // 字符串值便捷取用（非字符串/缺键返回 fallback）
    std::string getStr(const std::string& key, const std::string& fallback = "") const {
        const Json* j = get(key);
        return j && j->isStr() ? j->str : fallback;
    }
    double getNum(const std::string& key, double fallback = 0) const {
        const Json* j = get(key);
        return j && j->isNum() ? j->num : fallback;
    }

    static Json parse(const std::string& text, bool* ok = nullptr); // 定义在下方
    std::string dump() const;                                       // 定义在下方
};

namespace json_detail {

inline void skipWs(const char*& p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
}

inline bool parseHex4(const char*& p, const char* end, unsigned& out) {
    if (end - p < 4) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
        char c = *p++;
        out <<= 4;
        if (c >= '0' && c <= '9') out |= unsigned(c - '0');
        else if (c >= 'a' && c <= 'f') out |= unsigned(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') out |= unsigned(c - 'A' + 10);
        else return false;
    }
    return true;
}

inline void appendUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80) {
        s += char(cp);
    } else if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6));
        s += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += char(0xE0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xF0 | (cp >> 18));
        s += char(0x80 | ((cp >> 12) & 0x3F));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
}

inline bool parseString(const char*& p, const char* end, std::string& out) {
    if (p >= end || *p != '"') return false;
    ++p;
    out.clear();
    while (p < end) {
        char c = *p;
        if (c == '"') { ++p; return true; }
        if (c == '\\') {
            ++p;
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!parseHex4(p, end, cp)) return false;
                    // 代理对
                    if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        unsigned lo = 0;
                        if (!parseHex4(p, end, lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        } else {
            out += c;
            ++p;
        }
    }
    return false;
}

inline bool parseValue(const char*& p, const char* end, Json& out);

inline bool parseArray(const char*& p, const char* end, Json& out) {
    ++p; // 跳过 [
    out = Json::makeArr();
    skipWs(p, end);
    if (p < end && *p == ']') { ++p; return true; }
    for (;;) {
        skipWs(p, end);
        Json v;
        if (!parseValue(p, end, v)) return false;
        out.arr.push_back(std::move(v));
        skipWs(p, end);
        if (p < end && *p == ',') { ++p; continue; }
        if (p < end && *p == ']') { ++p; return true; }
        return false;
    }
}

inline bool parseObject(const char*& p, const char* end, Json& out) {
    ++p; // 跳过 {
    out = Json::makeObj();
    skipWs(p, end);
    if (p < end && *p == '}') { ++p; return true; }
    for (;;) {
        skipWs(p, end);
        std::string key;
        if (!parseString(p, end, key)) return false;
        skipWs(p, end);
        if (p >= end || *p != ':') return false;
        ++p;
        skipWs(p, end);
        Json v;
        if (!parseValue(p, end, v)) return false;
        out.obj.emplace_back(std::move(key), std::move(v));
        skipWs(p, end);
        if (p < end && *p == ',') { ++p; continue; }
        if (p < end && *p == '}') { ++p; return true; }
        return false;
    }
}

inline bool parseValue(const char*& p, const char* end, Json& out) {
    skipWs(p, end);
    if (p >= end) return false;
    char c = *p;
    if (c == '{') return parseObject(p, end, out);
    if (c == '[') return parseArray(p, end, out);
    if (c == '"') return parseString(p, end, out.str) && (out.type = Json::Type::Str, true);
    if (c == 't' && end - p >= 4 && strncmp(p, "true", 4) == 0) { p += 4; out = Json::makeBool(true); return true; }
    if (c == 'f' && end - p >= 5 && strncmp(p, "false", 5) == 0) { p += 5; out = Json::makeBool(false); return true; }
    if (c == 'n' && end - p >= 4 && strncmp(p, "null", 4) == 0) { p += 4; out = Json(); return true; }
    // 数值（含负号/指数；strtod 自带推进）
    {
        char* numEnd = nullptr;
        double v = strtod(p, &numEnd);
        if (numEnd == p) return false;
        p = numEnd;
        out = Json::makeNum(v);
        return true;
    }
}

inline void dumpEscaped(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    out += '"';
}

} // namespace json_detail

inline Json Json::parse(const std::string& text, bool* ok) {
    const char* p = text.data();
    const char* end = p + text.size();
    json_detail::skipWs(p, end);
    Json j;
    bool success = json_detail::parseValue(p, end, j);
    if (success) {
        json_detail::skipWs(p, end);
        success = p == end; // 尾部垃圾判定
    }
    if (ok) *ok = success;
    return success ? j : Json();
}

inline std::string Json::dump() const {
    using namespace json_detail;
    switch (type) {
        case Type::Null: return "null";
        case Type::Bool: return b ? "true" : "false";
        case Type::Num: {
            // 整数值不带小数点（协议 id 对齐），浮点用最短可靠表示
            if (std::floor(num) == num && std::abs(num) < 1e15) {
                char buf[32];
                snprintf(buf, sizeof buf, "%lld", static_cast<long long>(num));
                return buf;
            }
            char buf[32];
            snprintf(buf, sizeof buf, "%.17g", num);
            return buf;
        }
        case Type::Str: {
            std::string out;
            dumpEscaped(str, out);
            return out;
        }
        case Type::Arr: {
            std::string out = "[";
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i) out += ",";
                out += arr[i].dump();
            }
            out += "]";
            return out;
        }
        case Type::Obj: {
            std::string out = "{";
            for (size_t i = 0; i < obj.size(); ++i) {
                if (i) out += ",";
                dumpEscaped(obj[i].first, out);
                out += ":";
                out += obj[i].second.dump();
            }
            out += "}";
            return out;
        }
    }
    return "null";
}

// ---------------- 工具注册表 ----------------

struct ToolResult {
    std::string output;            // 给模型看的文本
    Json data = Json();            // 可选结构化输出（null = 无）
    bool ok = true;                // 工具级成败（false 时 output 作为错误文本）
};

using ToolHandler = std::function<ToolResult(const Json&)>;

struct ToolDef {
    std::string name;        // 裸名（宿主注册时自动加 {agent}_ 前缀）
    std::string description;
    Json parameters;         // JSON Schema（type/properties/required），原样透传给模型
    ToolHandler handler;
};

class ToolRegistry {
public:
    void add(ToolDef def) {
        for (auto& d : tools_)
            if (d.name == def.name) { d = std::move(def); return; }
        tools_.push_back(std::move(def));
    }
    const std::vector<ToolDef>& list() const { return tools_; }
    const ToolDef* find(const std::string& name) const {
        for (auto& d : tools_)
            if (d.name == name) return &d;
        return nullptr;
    }

private:
    std::vector<ToolDef> tools_;
};

// 全局注册表（main.cpp 里 REGISTER_TOOL 宏展开后填充；子代理项目专属工具）
inline ToolRegistry& registry() {
    static ToolRegistry inst;
    return inst;
}

// 项目工具注册：REGISTER_TOOL(name, desc, schemaText, fn)——fn 为无逗号 lambda 或函数指针。
// 注意：预处理器按裸逗号拆参——schema JSON 含数组逗号（required 列表）或 fn 带 lambda 参数列表
// 逗号时会被拆断。安全用法：工具逻辑写普通函数/无捕获 lambda 后取地址，schema 用无逗号 JSON
// （required 省略）；或直接在 main() 前手动注册：
//   gebai::registry().add({"eval", "描述", gebai::Json::parse(SCHEMA), &toolEval});
#define REGISTER_TOOL(name, desc, schemaText, fn)                                          \
    static bool gebai_registered_##name = [] {                                             \
        bool schemaOk = false;                                                             \
        gebai::Json schema = gebai::Json::parse(schemaText, &schemaOk);                    \
        if (!schemaOk) {                                                                   \
            std::fprintf(stderr, "[framework] tool %s schema invalid JSON\n", name);        \
            return false;                                                                 \
        }                                                                                  \
        gebai::registry().add({name, desc, std::move(schema), fn});                        \
        return true;                                                                      \
    }()

// ---------------- 请求级上下文（协议 v2） ----------------

/** 请求级 ctx：边车为进程单例跨会话共享，宿主每次 tool.call 携带——会话工作目录（相对路径
 *  解析基准）、任务级 env 覆盖、会话标识。分发循环串行执行，当前请求 ctx 在工具 handler
 *  运行前设置（g_currentCtx）。 */
struct SidecarCtx {
    std::string sessionId;
    std::string user;
    std::string cwd;
    std::map<std::string, std::string> env;
    bool sandboxed = false;
};

inline SidecarCtx g_currentCtx;
inline bool g_hasCtx = false;

inline SidecarCtx ctx() { return g_currentCtx; }

/** 请求级环境变量：先查任务 env 覆盖，再回落进程 env。禁止写 setenv/putenv（并发请求不同
 *  会话互踩，进程全局态承载不了请求级数据）。 */
inline std::string ctxEnv(const std::string& key, const std::string& fallback = "") {
    auto it = g_currentCtx.env.find(key);
    if (g_hasCtx && it != g_currentCtx.env.end()) return it->second;
    const char* v = std::getenv(key.c_str());
    return v ? v : fallback;
}

/** 路径解析：绝对路径原样；相对路径基准=当前请求 ctx.cwd（会话工作区）；无请求 ctx 时
 *  回落进程工作目录。 */
inline std::string ctxResolve(const std::string& p) {
    if (p.empty()) return p;
    bool abs = !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 2 && p[1] == ':' && (p[2] == '/' || p[2] == '\\')));
    if (abs) return p;
    std::string base = g_hasCtx ? g_currentCtx.cwd : "";
    if (base.empty()) {
        char wd[4096];
#ifdef _WIN32
        if (_getcwd(wd, sizeof(wd))) base = wd;
#else
        if (getcwd(wd, sizeof(wd))) base = wd;
#endif
    }
    if (base.empty()) return p;
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) base.pop_back();
    char sep = base.find('\\') != std::string::npos ? '\\' : '/';
    std::string rel = p;
    while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) rel.erase(rel.begin());
    return base + sep + rel;
}

inline SidecarCtx parseCtx(const Json* j) {
    SidecarCtx c;
    if (!j) return c;
    c.sessionId = j->getStr("sessionId");
    c.user = j->getStr("user");
    c.cwd = j->getStr("cwd");
    if (const Json* v = j->get("sandboxed")) c.sandboxed = v->isBool() && v->b;
    if (const Json* v = j->get("env")) {
        if (v->isObj()) {
            for (const auto& kv : v->obj) {
                if (kv.second.isStr()) c.env[kv.first] = kv.second.str;
            }
        }
    }
    return c;
}

// ---------------- 协议主循环 ----------------

namespace protocol {

/** 子代理项目名：{agent_dir} 目录名（宿主要求 init.name 与 manifest.name 一致；尾部空白/斜杠防御性剔除）。 */
inline std::string agentName() {
    const char* dir = std::getenv("GEBAI_AGENT_DIR");
    if (!dir || !*dir) return "cpp";
    std::string s = dir;
    while (!s.empty() && (s.back() == '/' || s.back() == '\\' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t pos = s.find_last_of("/\\");
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

inline Json toolsListJson(const ToolRegistry& reg) {
    Json arr = Json::makeArr();
    for (auto& d : reg.list()) {
        Json t = Json::makeObj();
        t.set("name", Json::makeStr(d.name));
        t.set("description", Json::makeStr(d.description));
        t.set("parameters", d.parameters);
        arr.arr.push_back(std::move(t));
    }
    return arr;
}

inline Json toolCall(const ToolRegistry& reg, const std::string& tool, const Json& callArgs, const Json* ctxJson) {
    const ToolDef* def = reg.find(tool);
    if (!def) {
        Json err = Json::makeObj();
        err.set("error", Json::makeStr("未知工具: " + tool));
        return err;
    }
    // 请求级 ctx（协议 v2）：串行分发循环内设置，工具 handler 经 ctx()/ctxEnv()/ctxResolve() 读取
    g_currentCtx = parseCtx(ctxJson);
    g_hasCtx = ctxJson != nullptr;
    ToolResult r;
    try {
        r = def->handler(callArgs);
    } catch (const std::exception& e) {
        r = ToolResult{};
        r.ok = false;
        r.output = std::string("工具异常: ") + e.what();
    } catch (...) {
        r = ToolResult{};
        r.ok = false;
        r.output = "工具异常: 未知错误";
    }
    g_hasCtx = false;
    Json result = Json::makeObj();
    result.set("output", Json::makeStr(r.output));
    if (r.ok && !r.data.isNull()) result.set("data", r.data);
    return result;
}

/** 写一行协议响应并 flush（stdout 唯一写入点）。 */
inline void respond(const Json& resp) {
    std::string line = resp.dump();
    fwrite(line.data(), 1, line.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

/** NDJSON 主循环（stdin EOF → 返回 → main 里退出）。 */
inline void run() {
#ifdef _WIN32
    // Windows 控制台默认 GBK：切 UTF-8（失败不致命，英文 ASCII 不受影响）
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    std::string line;
    std::vector<char> buf(1 << 16);
    while (true) {
        // 手写行读取（getline 对超长行/二进制安全）
        line.clear();
        bool eof = false;
        while (true) {
            if (!fgets(buf.data(), int(buf.size()), stdin)) { eof = true; break; }
            line += buf.data();
            size_t len = strlen(buf.data());
            if (len > 0 && buf.data()[len - 1] == '\n') break;
            if (len + 1 < buf.size()) break; // 行结束（无换行结尾的最后一行）
        }
        if (eof && line.empty()) break;
        // 去行尾
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        bool ok = false;
        Json req = Json::parse(line, &ok);
        Json resp = Json::makeObj();
        if (!ok) {
            resp.set("id", Json());
            resp.set("ok", Json::makeBool(false));
            resp.set("error", Json::makeStr("请求解析失败: " + line.substr(0, 200)));
            respond(resp);
            continue;
        }
        long long id = req.getNum("id", -1);
        std::string op = req.getStr("op");
        resp.set("id", id >= 0 ? Json::makeNum(double(id)) : Json());
        std::string tool = req.getStr("tool");
        const Json* args = req.get("args");
        Json callArgs = args ? *args : Json();
        const Json* ctxJson = req.get("ctx");
        bool isErr = false;
        Json result;
        if (op == "init") {
            Json info = Json::makeObj();
            info.set("name", Json::makeStr(agentName()));
            info.set("protocol", Json::makeNum(2));
            info.set("lang", Json::makeStr("cpp"));
#ifdef _WIN32
            info.set("platform", Json::makeStr("win32"));
#else
            info.set("platform", Json::makeStr("unix"));
#endif
            result = info;
        } else if (op == "tools.list") {
            result = toolsListJson(registry());
        } else if (op == "tool.call") {
            result = toolCall(registry(), tool, callArgs, ctxJson);
            if (result.get("error")) isErr = true;
        } else {
            isErr = true;
            result = Json::makeStr("未知操作: " + op);
        }
        if (isErr) {
            resp.set("ok", Json::makeBool(false));
            const Json* e = result.isStr() ? &result : result.get("error");
            resp.set("error", Json::makeStr(e ? (e->isStr() ? e->str : e->dump()) : "未知错误"));
        } else {
            resp.set("ok", Json::makeBool(true));
            resp.set("result", result);
        }
        respond(resp);
    }
}

} // namespace protocol

/** 便捷：main() 里直接 `return gebai::main();`（0 退出码）。 */
inline int main() {
    protocol::run();
    return 0;
}

} // namespace gebai
