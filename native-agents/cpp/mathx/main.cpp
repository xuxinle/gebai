// mathx 子代理项目：高性能数学表达式求值（C++ 实现，边车常驻进程）。
// 基于语言目录共享基础框架（native-agents/cpp/framework.hpp），本文件只写工具逻辑——
// 框架处理协议（init/tools.list/tool.call + NDJSON 行循环）。
// 工具注册采用「普通函数 + main() 前手动注册」形式（REGISTER_TOOL 宏对含逗号的
// schema/lambda 有预处理器拆参陷阱，见 framework.hpp 注释）。
#include "../framework.hpp"

#include <map>
#include <stdexcept>

namespace {

// ---------------- 递归下降表达式求值器 ----------------

struct EvalError : std::runtime_error {
    explicit EvalError(const std::string& msg) : std::runtime_error(msg) {}
};

class Evaluator {
public:
    explicit Evaluator(std::string src, std::map<std::string, double> vars)
        : src_(std::move(src)), vars_(std::move(vars)) {}

    double eval() {
        pos_ = 0;
        skipWs();
        double v = parseExpr();
        skipWs();
        if (pos_ < src_.size()) throw EvalError("多余输入: '" + src_.substr(pos_, 16) + "'");
        return v;
    }

private:
    void skipWs() {
        while (pos_ < src_.size() && unsigned char(src_[pos_]) <= ' ') ++pos_;
    }
    char peek() {
        skipWs();
        return pos_ < src_.size() ? src_[pos_] : '\0';
    }
    bool eat(char c) {
        if (peek() == c) { ++pos_; return true; }
        return false;
    }
    [[noreturn]] void fail(const std::string& why) {
        throw EvalError("位置 " + std::to_string(pos_) + " 附近: " + why);
    }

    // 表达式 ::= 项 (('+'|'-') 项)*
    double parseExpr() {
        double v = parseTerm();
        for (;;) {
            if (eat('+')) v += parseTerm();
            else if (eat('-')) v -= parseTerm();
            else return v;
        }
    }
    // 项 ::= 一元元 (('*'|'/'|'%') 一元元)*
    double parseTerm() {
        double v = parseUnary();
        for (;;) {
            if (eat('*')) v *= parseUnary();
            else if (eat('/')) {
                double d = parseUnary();
                if (d == 0) fail("除零");
                v /= d;
            } else if (eat('%')) {
                double d = parseUnary();
                long long a = llround(v), b = llround(d);
                if (b == 0) fail("模零");
                v = double(a % b);
            } else return v;
        }
    }
    // 一元元 ::= ('+'|'-') 一元元 | 幂
    double parseUnary() {
        if (eat('-')) return -parseUnary();
        if (eat('+')) return parseUnary();
        return parsePower();
    }
    // 幂 ::= 基数 ('^' 一元元)?   （右结合）
    double parsePower() {
        double base = parseAtom();
        if (eat('^')) {
            double e = parseUnary(); // 右结合：2^3^2 = 2^(3^2)
            return std::pow(base, e);
        }
        return base;
    }
    // 基数 ::= 数字 | 标识符 | 标识符 '(' 参数 ')' | '(' 表达式 ')'
    double parseAtom() {
        skipWs();
        if (pos_ >= src_.size()) fail("表达式意外结束");
        char c = src_[pos_];
        if (c == '(') {
            ++pos_;
            double v = parseExpr();
            if (!eat(')')) fail("缺右括号");
            return v;
        }
        if (std::isdigit(unsigned char(c)) || c == '.') return parseNumber();
        if (std::isalpha(unsigned char(c)) || c == '_') return parseIdent();
        fail(std::string("意外字符 '") + c + "'");
    }
    double parseNumber() {
        char* end = nullptr;
        double v = strtod(src_.c_str() + pos_, &end);
        if (end == src_.c_str() + pos_) fail("数字解析失败");
        pos_ = size_t(end - src_.c_str());
        return v;
    }
    double parseIdent() {
        size_t start = pos_;
        while (pos_ < src_.size() && (std::isalnum(unsigned char(src_[pos_])) || src_[pos_] == '_')) ++pos_;
        std::string name = src_.substr(start, pos_ - start);
        if (eat('(')) {
            // 函数调用：内置数学函数 + 阶乘
            std::vector<double> args;
            if (peek() != ')') {
                args.push_back(parseExpr());
                while (eat(',')) args.push_back(parseExpr());
            }
            if (!eat(')')) fail("函数缺右括号: " + name);
            return callFn(name, args);
        }
        auto it = vars_.find(name);
        if (it != vars_.end()) return it->second;
        if (name == "pi") return 3.14159265358979323846;
        if (name == "e") return 2.71828182845904523536;
        throw EvalError("未知标识符: " + name);
    }
    double callFn(const std::string& name, const std::vector<double>& args) {
        auto need = [&](size_t n) {
            if (args.size() != n) throw EvalError(name + " 需要 " + std::to_string(n) + " 个参数，得到 " + std::to_string(args.size()));
        };
        if (name == "abs") { need(1); return std::abs(args[0]); }
        if (name == "sqrt") { need(1); return std::sqrt(args[0]); }
        if (name == "sin") { need(1); return std::sin(args[0]); }
        if (name == "cos") { need(1); return std::cos(args[0]); }
        if (name == "tan") { need(1); return std::tan(args[0]); }
        if (name == "asin") { need(1); return std::asin(args[0]); }
        if (name == "acos") { need(1); return std::acos(args[0]); }
        if (name == "atan") { need(1); return std::atan(args[0]); }
        if (name == "atan2") { need(2); return std::atan2(args[0], args[1]); }
        if (name == "ln") { need(1); return std::log(args[0]); }
        if (name == "log") { need(1); return std::log10(args[0]); }
        if (name == "exp") { need(1); return std::exp(args[0]); }
        if (name == "floor") { need(1); return std::floor(args[0]); }
        if (name == "ceil") { need(1); return std::ceil(args[0]); }
        if (name == "round") { need(1); return std::round(args[0]); }
        if (name == "min") { need(2); return std::min(args[0], args[1]); }
        if (name == "max") { need(2); return std::max(args[0], args[1]); }
        if (name == "fact") {
            need(1);
            long long n = llround(args[0]);
            if (n < 0 || n > 170) throw EvalError("fact 定义域 [0,170]");
            double r = 1;
            for (long long i = 2; i <= n; ++i) r *= double(i);
            return r;
        }
        throw EvalError("未知函数: " + name);
    }

    std::string src_;
    size_t pos_ = 0;
    std::map<std::string, double> vars_;
};

// 变量参数（{"x":1,"y":2}）→ 变量表
std::map<std::string, double> parseVars(const gebai::Json* j) {
    std::map<std::string, double> vars;
    if (!j || !j->isObj()) return vars;
    for (auto& [k, v] : j->obj)
        if (v.isNum()) vars[k] = v.num;
    return vars;
}

std::string fmtNum(double v) {
    if (std::floor(v) == v && std::abs(v) < 1e15) {
        char buf[32];
        snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
        return buf;
    }
    char buf[40];
    snprintf(buf, sizeof buf, "%.12g", v);
    return buf;
}

// ---------------- 工具实现（普通函数，main 前集中注册） ----------------

gebai::ToolResult toolEval(const gebai::Json& args) {
    std::string expr = args.getStr("expression");
    if (expr.empty()) {
        gebai::ToolResult r;
        r.ok = false;
        r.output = "expression 不能为空";
        return r;
    }
    try {
        Evaluator ev(expr, parseVars(args.get("vars")));
        double v = ev.eval();
        gebai::ToolResult r;
        r.output = fmtNum(v);
        r.data = gebai::Json::makeObj();
        r.data.set("value", gebai::Json::makeNum(v));
        return r;
    } catch (const std::exception& e) {
        gebai::ToolResult r;
        r.ok = false;
        r.output = std::string("求值失败: ") + e.what();
        return r;
    }
}

gebai::ToolResult toolEvalBatch(const gebai::Json& args) {
    const gebai::Json* exprs = args.get("expressions");
    if (!exprs || !exprs->isArr()) {
        gebai::ToolResult r;
        r.ok = false;
        r.output = "expressions 须为字符串数组";
        return r;
    }
    auto vars = parseVars(args.get("vars"));
    std::vector<std::string> lines;
    gebai::Json results = gebai::Json::makeArr();
    int fails = 0;
    for (auto& e : exprs->arr) {
        std::string expr = e.isStr() ? e.str : e.dump();
        gebai::Json item = gebai::Json::makeObj();
        try {
            Evaluator ev(expr, vars);
            double v = ev.eval();
            item.set("ok", gebai::Json::makeBool(true));
            item.set("value", gebai::Json::makeNum(v));
            lines.push_back(expr + " = " + fmtNum(v));
        } catch (const std::exception& ex) {
            item.set("ok", gebai::Json::makeBool(false));
            item.set("error", gebai::Json::makeStr(ex.what()));
            lines.push_back(expr + " ! " + ex.what());
            ++fails;
        }
        results.arr.push_back(std::move(item));
    }
    gebai::ToolResult r;
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) joined += "\n";
        joined += lines[i];
    }
    r.output = std::to_string(exprs->arr.size() - fails) + "/" + std::to_string(exprs->arr.size()) + " 条成功:\n" + joined;
    r.data = gebai::Json::makeObj();
    r.data.set("results", results);
    return r;
}

gebai::ToolResult toolStats(const gebai::Json& args) {
    const gebai::Json* vals = args.get("values");
    if (!vals || !vals->isArr() || vals->arr.empty()) {
        gebai::ToolResult r;
        r.ok = false;
        r.output = "values 须为非空数值数组";
        return r;
    }
    std::vector<double> v;
    v.reserve(vals->arr.size());
    for (auto& e : vals->arr)
        if (e.isNum()) v.push_back(e.num);
    if (v.empty()) {
        gebai::ToolResult r;
        r.ok = false;
        r.output = "values 内无数值";
        return r;
    }
    double sum = 0;
    double mn = v[0], mx = v[0];
    for (double x : v) {
        sum += x;
        mn = std::min(mn, x);
        mx = std::max(mx, x);
    }
    double mean = sum / double(v.size());
    std::vector<double> sorted = v;
    std::sort(sorted.begin(), sorted.end());
    double median = sorted.size() % 2 ? sorted[sorted.size() / 2]
                                      : (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]) / 2;
    double sq = 0;
    for (double x : v) sq += (x - mean) * (x - mean);
    double stdev = v.size() > 1 ? std::sqrt(sq / double(v.size() - 1)) : 0;
    gebai::ToolResult r;
    r.data = gebai::Json::makeObj();
    r.data.set("count", gebai::Json::makeNum(double(v.size())));
    r.data.set("sum", gebai::Json::makeNum(sum));
    r.data.set("mean", gebai::Json::makeNum(mean));
    r.data.set("min", gebai::Json::makeNum(mn));
    r.data.set("max", gebai::Json::makeNum(mx));
    r.data.set("median", gebai::Json::makeNum(median));
    r.data.set("stdev", gebai::Json::makeNum(stdev));
    r.output = "count: " + fmtNum(double(v.size())) + "\nsum: " + fmtNum(sum) + "\nmean: " + fmtNum(mean)
             + "\nmin: " + fmtNum(mn) + "\nmax: " + fmtNum(mx) + "\nmedian: " + fmtNum(median)
             + "\nstdev: " + fmtNum(stdev);
    return r;
}

} // namespace

// ---------------- 工具 schema（无逗号 JSON 或经手动注册均可） ----------------

namespace {

// 完整 schema（含 required 数组逗号）——用手动注册路径
const char* SCHEMA_EVAL = R"json({
    "type": "object",
    "properties": {
        "expression": {"type": "string", "description": "数学表达式，如 \"(x^2 + y^2) * sin(pi/6)\""},
        "vars": {"type": "object", "description": "变量表 {名字:数值}", "additionalProperties": {"type": "number"}}
    },
    "required": ["expression"]
})json";

const char* SCHEMA_BATCH = R"json({
    "type": "object",
    "properties": {
        "expressions": {"type": "array", "items": {"type": "string"}, "description": "表达式列表"},
        "vars": {"type": "object", "description": "变量表（全部表达式共用）", "additionalProperties": {"type": "number"}}
    },
    "required": ["expressions"]
})json";

const char* SCHEMA_STATS = R"json({
    "type": "object",
    "properties": {
        "values": {"type": "array", "items": {"type": "number"}, "description": "数值列表"}
    },
    "required": ["values"]
})json";

} // namespace

int main() {
    // 集中注册（规避 REGISTER_TOOL 宏对逗号 schema/lambda 的拆参陷阱）
    gebai::registry().add({"eval",
        "求值数学表达式（常驻进程，纳秒级开销）：支持 + - * / % ^、括号、变量代入（vars 传变量表）、"
        "常量 pi/e、内置函数 abs sqrt sin cos tan asin acos atan atan2 ln log exp floor ceil round min max fact。"
        "错误返回原因与位置（如除零、未知函数）。",
        gebai::Json::parse(SCHEMA_EVAL), &toolEval});
    gebai::registry().add({"eval_batch",
        "批量求值：一次传多个表达式（同变量表），返回逐条结果——适合批量计算/表驱动场景（一次调用省多次往返）。",
        gebai::Json::parse(SCHEMA_BATCH), &toolEvalBatch});
    gebai::registry().add({"stats",
        "统计一批数值：count/sum/mean/min/max/median/stdev（样本标准差），常驻进程适合大数组反复计算。",
        gebai::Json::parse(SCHEMA_STATS), &toolStats});
    return gebai::main();
}
