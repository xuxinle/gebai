// Package framework —— 歌白多语言子代理 Rust/Go 系基础框架（协议 v1，见 native-agents/README.md）。
//
// Go 语言目录 native-agents/go/ 下共享本包；每个子代理项目（语言目录下的二级目录）一个
// main.go —— import "gebai/native-framework/framework" 后用 RegisterTool 注册专属工具 +
// main() 调用 framework.Run() 即成完整边车驱动。一种语言派生任意多个子代理，实现语言对模型
// 透明（模型只看到工具与提示词）。
//
// 协议（stdin/stdout 各一行一个 JSON，UTF-8）：
//
//	{"id":1,"op":"init"}       → {"id":1,"ok":true,"result":{"name":"<项目目录名>","protocol":1,...}}
//	{"id":2,"op":"tools.list"} → {"id":2,"ok":true,"result":[{name,description,parameters},...]}
//	{"id":3,"op":"tool.call","args":{"tool":"now","args":{...}}}
//	                           → {"id":3,"ok":true,"result":{"output":"...","data":{...}}}
//
// 约定：stdout 只写协议行（fmt.Println 全部禁用，调试输出用 os.Stderr）；stdin EOF → 立即
// 退出（父进程已死，防孤儿进程）。
//
// 宿主注入的运行上下文（环境变量）：GEBAI_HOME（数据根）、GEBAI_AGENT_DIR（子代理项目目录）。
package framework

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
)

// ToolDef —— 子代理工具：裸名（宿主注册时自动加 {agent}_ 前缀）+ 描述 + JSON Schema 参数。
type ToolDef struct {
	Name        string
	Description string
	Parameters  map[string]any
	Execute     func(args map[string]any) ToolResult
}

// ToolResult —— 工具执行结果：Output 给模型看的文本，Data 可选结构化输出（透传给 js/agent_run 等）。
type ToolResult struct {
	Output string
	Data   any
}

// ToolOk / ToolErr —— 结果构造器。
func ToolOk(output string, data any) ToolResult { return ToolResult{Output: output, Data: data} }
func ToolErr(output string) ToolResult          { return ToolResult{Output: output} }

var (
	mu    sync.Mutex
	tools = map[string]*ToolDef{}
	order []string // 注册序（tools.list 输出稳定）
)

// RegisterTool —— 注册工具（同名重复注册覆盖，order 保持首次位置）。
func RegisterTool(t *ToolDef) {
	mu.Lock()
	defer mu.Unlock()
	if _, dup := tools[t.Name]; !dup {
		order = append(order, t.Name)
	}
	tools[t.Name] = t
}

// AgentName —— 子代理项目名：{agent_dir} 目录名（宿主要求 init.name 与 manifest.name 一致；
// 尾部空白/斜杠防御性剔除）。
func AgentName() string {
	dir := os.Getenv("GEBAI_AGENT_DIR")
	if dir == "" {
		return "go"
	}
	dir = strings.TrimRight(dir, `/\ `+"\t")
	if i := strings.LastIndexAny(dir, `/\`); i >= 0 {
		dir = dir[i+1:]
	}
	return dir
}

// Run —— 协议主循环：逐行读 stdin 请求（NDJSON），按 op 分发，单行应答 stdout。
// 到 EOF 即返回（调用方随即退出——父进程已死）。
func Run() {
	sc := bufio.NewScanner(os.Stdin)
	sc.Buffer(make([]byte, 1024*1024), 16*1024*1024) // 单请求上限 16MB（大数组/长文本参数）
	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		resp := dispatch(line)
		b, err := json.Marshal(resp)
		if err != nil {
			b = []byte(fmt.Sprintf(`{"id":null,"ok":false,"error":"响应序列化失败: %s"}`, err.Error()))
		}
		w.Write(b)
		w.WriteByte('\n')
		w.Flush()
	}
}

// dispatch —— 单请求处理：解析 → 分发 init/tools.list/tool.call → 组应答。
func dispatch(line string) map[string]any {
	var req struct {
		ID   any            `json:"id"`
		Op   string         `json:"op"`
		Args map[string]any `json:"args"`
	}
	if err := json.Unmarshal([]byte(line), &req); err != nil {
		return map[string]any{"id": nil, "ok": false, "error": "请求解析失败: " + err.Error()}
	}
	mk := func(ok bool, result any, errMsg string) map[string]any {
		m := map[string]any{"id": req.ID, "ok": ok}
		if ok {
			m["result"] = result
		} else {
			m["error"] = errMsg
		}
		return m
	}
	switch req.Op {
	case "init":
		return mk(true, map[string]any{
			"name":     AgentName(),
			"protocol": 1,
			"lang":     "go",
			"platform": runtimeGOOS(),
			"go":       runtimeVersion(),
		}, "")
	case "tools.list":
		mu.Lock()
		defer mu.Unlock()
		list := make([]map[string]any, 0, len(order))
		for _, name := range order {
			t := tools[name]
			params := t.Parameters
			if params == nil {
				params = map[string]any{"type": "object", "properties": map[string]any{}}
			}
			list = append(list, map[string]any{"name": t.Name, "description": t.Description, "parameters": params})
		}
		return mk(true, list, "")
	case "tool.call":
		name, _ := req.Args["tool"].(string)
		mu.Lock()
		t := tools[name]
		mu.Unlock()
		if t == nil {
			return mk(false, nil, fmt.Sprintf("未知工具: %s", name))
		}
		args, _ := req.Args["args"].(map[string]any)
		if args == nil {
			args = map[string]any{}
		}
		res := safeExecute(t.Execute, args)
		m := map[string]any{"output": res.Output}
		if res.Data != nil {
			m["data"] = res.Data
		}
		return mk(true, m, "")
	default:
		return mk(false, nil, "未知 op: "+req.Op)
	}
}

// safeExecute —— 工具执行 panic 兜底（不让单次工具错误杀死常驻进程）。
func safeExecute(fn func(map[string]any) ToolResult, args map[string]any) (res ToolResult) {
	defer func() {
		if r := recover(); r != nil {
			res = ToolErr(fmt.Sprintf("工具内部错误（panic 已恢复）: %v", r))
		}
	}()
	return fn(args)
}

// StrArg / NumArg / BoolArg —— 参数取值助手（缺省/类型不符返回零值）。
func StrArg(args map[string]any, key string) string {
	s, _ := args[key].(string)
	return s
}

func NumArg(args map[string]any, key string) float64 {
	switch v := args[key].(type) {
	case float64:
		return v
	case json.Number:
		f, _ := v.Float64()
		return f
	}
	return 0
}

// StrListArg —— 字符串列表参数（JSON 数组）。
func StrListArg(args map[string]any, key string) []string {
	raw, _ := args[key].([]any)
	out := make([]string, 0, len(raw))
	for _, v := range raw {
		if s, ok := v.(string); ok {
			out = append(out, s)
		}
	}
	return out
}

// SchemaProp —— JSON Schema 属性构造（type + description）。
func SchemaProp(typ, desc string) map[string]any {
	return map[string]any{"type": typ, "description": desc}
}

// SchemaRequire —— required 数组构造。
func SchemaRequire(names ...string) []string { return names }

// AgentDir —— 本子代理项目目录（GEBAI_AGENT_DIR，缺省取当前目录）。
func AgentDir() string {
	if d := os.Getenv("GEBAI_AGENT_DIR"); d != "" {
		return d
	}
	if wd, err := os.Getwd(); err == nil {
		return wd
	}
	return "."
}

// LangDir —— 语言目录（agent_dir 上一级——本框架包所在模块根）。
func LangDir() string { return filepath.Dir(AgentDir()) }

var _ = io.EOF // 保留 io 引用（未来扩展用）

// runtimeGOOS / runtimeVersion —— init 上报用（平台与 Go 版本）。
func runtimeGOOS() string      { return runtime.GOOS }
func runtimeVersion() string   { return runtime.Version() }
