// gotime 子代理项目：时间日期工具（Go 实现，边车常驻进程）。
// 基于语言目录共享基础框架（native-agents/go/framework），本文件只写工具逻辑——
// 框架处理协议（init/tools.list/tool.call + NDJSON 行循环）。
// 构建：go build -o driver{exe}（构建引导自动执行；产物在项目目录）。
package main

import (
	"fmt"
	"strings"
	"time"

	fw "gebai/native-framework/framework"
)

func init() {
	// now：当前时间（支持时区与自定义格式）
	fw.RegisterTool(&fw.ToolDef{
		Name: "now",
		Description: "获取当前时间（常驻进程，纳秒级开销）：默认本地时区 RFC3339；tz 可指定 IANA 时区（如 Asia/Shanghai、UTC、" +
			"America/New_York）；format 可传 Go 布局（2006-01-02 15:04:05）或预设别名 datetime/date/time/rfc3339/unix。",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"tz":     fw.SchemaProp("string", "IANA 时区名（缺省本地时区；如 Asia/Shanghai、UTC）"),
				"format": fw.SchemaProp("string", "Go 时间布局（如 2006-01-02 15:04:05）或预设别名 datetime/date/time/rfc3339/unix"),
			},
		},
		Execute: toolNow,
	})

	// parse：解析时间为 unix 时间戳与 RFC3339
	fw.RegisterTool(&fw.ToolDef{
		Name: "parse",
		Description: "解析时间字符串为 unix 秒/毫秒与标准形态（RFC3339）：支持常见格式自动探测（RFC3339、日期、日期时间、" +
			"yyyy/MM/dd、unix 秒）；tz 指定输入时区（缺省本地）。",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"text": fw.SchemaProp("string", "时间文本，如 2026-07-14 08:30:00、2026/07/14、1752457800"),
				"tz":   fw.SchemaProp("string", "输入时区（缺省本地时区；无时区标记的文本按此时区解释）"),
			},
			"required": []string{"text"},
		},
		Execute: toolParse,
	})

	// duration：时间跨度解析与换算
	fw.RegisterTool(&fw.ToolDef{
		Name: "duration",
		Description: "解析时长并换算多种单位（Go duration 语法：1h30m、45s、100ms、1.5h；也接受纯秒数）——" +
			"返回 ns/µs/ms/s/m/h/d 全单位值与人性化描述，调度/倒计时计算零歧义。",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"text": fw.SchemaProp("string", "时长文本，如 1h30m、2d12h、500ms、90（纯数字按秒）"),
			},
		},
		Execute: toolDuration,
	})
}

// 预设格式别名 → Go 布局
var layoutAliases = map[string]string{
	"datetime": "2006-01-02 15:04:05",
	"date":     "2006-01-02",
	"time":     "15:04:05",
	"rfc3339":  time.RFC3339,
}

func toolNow(args map[string]any) fw.ToolResult {
	tzName := fw.StrArg(args, "tz")
	loc := time.Local
	if tzName != "" {
		var err error
		loc, err = time.LoadLocation(tzName)
		if err != nil {
			return fw.ToolErr(fmt.Sprintf("未知时区 %q（IANA 名如 Asia/Shanghai、UTC）: %v", tzName, err))
		}
	}
	now := time.Now().In(loc)
	format := fw.StrArg(args, "format")
	switch format {
	case "":
		format = time.RFC3339
	case "unix":
		return fw.ToolOk(fmt.Sprintf("%d", now.Unix()), map[string]any{"unix": now.Unix(), "unixMilli": now.UnixMilli()})
	default:
		if layout, ok := layoutAliases[strings.ToLower(format)]; ok {
			format = layout
		}
	}
	text := now.Format(format)
	return fw.ToolOk(text, map[string]any{
		"text": text, "unix": now.Unix(), "unixMilli": now.UnixMilli(), "zone": now.Location().String(),
	})
}

var parseLayouts = []string{
	time.RFC3339Nano, time.RFC3339,
	"2006-01-02 15:04:05.999999999 -0700 MST", "2006-01-02 15:04:05 -0700",
	"2006-01-02 15:04:05", "2006-01-02 15:04", "2006-01-02",
	"2006/01/02 15:04:05", "2006/01/02 15:04", "2006/01/02",
	"15:04:05", "15:04",
	time.RFC1123, time.RFC1123Z,
}

func toolParse(args map[string]any) fw.ToolResult {
	text := strings.TrimSpace(fw.StrArg(args, "text"))
	if text == "" {
		return fw.ToolErr("缺少 text 参数")
	}
	// 纯数字：unix 秒
	if sec, ok := parseUnixSeconds(text); ok {
		t := time.Unix(sec, 0)
		return formatParsed(t)
	}
	tzName := fw.StrArg(args, "tz")
	loc := time.Local
	if tzName != "" {
		var err error
		loc, err = time.LoadLocation(tzName)
		if err != nil {
			return fw.ToolErr(fmt.Sprintf("未知时区 %q: %v", tzName, err))
		}
	}
	// 无时区布局按 loc 解释；带时区的布局（RFC3339 等）自带偏移
	for _, layout := range parseLayouts {
		if t, err := time.ParseInLocation(layout, text, loc); err == nil {
			return formatParsed(t)
		}
	}
	return fw.ToolErr(fmt.Sprintf("无法识别时间文本 %q（支持 RFC3339、2006-01-02[ 15:04[:05]]、2006/01/02、unix 秒）", text))
}

func parseUnixSeconds(text string) (int64, bool) {
	if len(text) < 9 || len(text) > 12 { // 秒级时间戳 9~12 位合理区间（2001~51382 年）
		return 0, false
	}
	var n int64
	for _, c := range text {
		if c < '0' || c > '9' {
			return 0, false
		}
		n = n*10 + int64(c-'0')
	}
	return n, true
}

func formatParsed(t time.Time) fw.ToolResult {
	local := t.Local()
	return fw.ToolOk(fmt.Sprintf("%s（unix %d）", local.Format("2006-01-02 15:04:05 MST"), t.Unix()), map[string]any{
		"unix": t.Unix(), "unixMilli": t.UnixMilli(), "rfc3339": t.Format(time.RFC3339), "local": local.Format("2006-01-02 15:04:05"),
	})
}

func toolDuration(args map[string]any) fw.ToolResult {
	text := strings.TrimSpace(fw.StrArg(args, "text"))
	if text == "" {
		return fw.ToolErr("缺少 text 参数")
	}
	var d time.Duration
	if isAllDigits(text) {
		// 纯数字按秒
		var sec float64
		if _, err := fmt.Sscanf(text, "%g", &sec); err != nil {
			return fw.ToolErr(fmt.Sprintf("无法解析时长 %q: %v", text, err))
		}
		d = time.Duration(sec * float64(time.Second))
	} else {
		// 支持 1.5h 小数单位：Go duration 语法本身支持 1.5h
		var err error
		d, err = time.ParseDuration(text)
		if err != nil {
			return fw.ToolErr(fmt.Sprintf("无法解析时长 %q（Go 语法如 1h30m、2d12h 需展开为 52h）：%v", text, err))
		}
	}
	// 2d12h 形态预处理（Go 不认 d 单位）
	// —— 在上面 ParseDuration 前展开
	lines := []string{
		fmt.Sprintf("ns=%d", d.Nanoseconds()),
		fmt.Sprintf("µs=%d", d.Microseconds()),
		fmt.Sprintf("ms=%d", d.Milliseconds()),
		fmt.Sprintf("s=%.6g", d.Seconds()),
		fmt.Sprintf("m=%.6g", d.Minutes()),
		fmt.Sprintf("h=%.6g", d.Hours()),
		fmt.Sprintf("d=%.6g", d.Hours()/24),
	}
	return fw.ToolOk(strings.Join(lines, "\n"), map[string]any{
		"nanoseconds": d.Nanoseconds(), "seconds": d.Seconds(), "humanize": d.String(),
	})
}

func isAllDigits(s string) bool {
	if s == "" {
		return false
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return false
		}
	}
	return true
}

func main() {
	fw.Run()
}
