// dirs 子代理项目：目录空间分析（Go 边车常驻进程）。
// 典型场景——磁盘空间分析（du/tree/top 大文件定位）：goroutine 并发遍历 + channel 聚合，
// 数十万文件目录亚秒级出结果——「哪个目录占空间/大文件在哪」一键定位。
// 基于语言目录共享基础框架（native-agents/go/framework），本文件只写工具逻辑。
// 构建：go build -o driver{exe}（构建引导自动执行；产物在项目目录）。
package main

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
	"sync/atomic"

	fw "gebai/native-framework/framework"
)

// 并发遍历：N 个 worker goroutine 消费目录队列，文件大小/数量/最深路径经 channel 聚合。
// 目录大小 = 递归子树文件总大小（du 语义）。

type fileInfo struct {
	path  string
	size  int64
	isDir bool
	depth int
}

// dirQueue —— 无界目录队列（互斥锁 + 条件变量）：worker 既是生产者又是消费者，
// 若用有界 channel，全部 worker 可能同时阻塞在「往队列发送」而无人在接收——结构性
// 自锁（目录突发多的树必现，如 node_modules）；无界队列发送永不阻塞，从根上消除。
type dirQueue struct {
	mu     sync.Mutex
	cond   *sync.Cond
	items  []string
	head   int
	closed bool
}

func newDirQueue() *dirQueue {
	q := &dirQueue{}
	q.cond = sync.NewCond(&q.mu)
	return q
}

func (q *dirQueue) push(s string) {
	q.mu.Lock()
	q.items = append(q.items, s)
	q.mu.Unlock()
	q.cond.Signal()
}

// close —— 唤醒全部等待者（它们见到空且已关则退出）。
func (q *dirQueue) close() {
	q.mu.Lock()
	q.closed = true
	q.mu.Unlock()
	q.cond.Broadcast()
}

// pop —— 取一个待处理目录；空且未关则等（pending>0 保证还有目录未完成，其完成路径
// 必经 push 或减到 0 后的 close 唤醒，不会永久等待）。
func (q *dirQueue) pop() (string, bool) {
	q.mu.Lock()
	defer q.mu.Unlock()
	for q.head >= len(q.items) && !q.closed {
		q.cond.Wait()
	}
	if q.head >= len(q.items) {
		return "", false
	}
	s := q.items[q.head]
	q.items[q.head] = ""
	q.head++
	if q.head == len(q.items) { // 队列耗尽：重置，防底层数组无限增长
		q.items = q.items[:0]
		q.head = 0
	}
	return s, true
}

// walkConcurrent —— 并发遍历目录树（worker 池 + 原子在途计数）：
// pending = 已入队未处理完的目录数；子目录入队前 Add、本目录全部发送完后减 1；
// 减到 0 的 worker 关队列。旧版有界 channel 下 worker 既是生产者又是消费者，全部 worker
// 可能同时阻塞在「往队列发送」而无人在接收——结构性自锁（目录突发多的树必现）；
// 无界队列 push 永不阻塞，从根上消除。不可读目录（权限等）记入 errs 跳过，不中断遍历。
func walkConcurrent(root string, maxDepth int) (entries []fileInfo, errs []string) {
	type dirResult struct {
		entries []fileInfo
		err     string
	}

	results := make(chan dirResult, 256)
	queue := newDirQueue()
	var pending atomic.Int64
	var workersWG sync.WaitGroup

	rootDepth := strings.Count(filepath.Clean(root), string(os.PathSeparator))
	depthOf := func(p string) int { return strings.Count(p, string(os.PathSeparator)) - rootDepth }

	pending.Add(1)
	queue.push(root)

	workers := runtime.NumCPU()
	if workers > 16 {
		workers = 16
	}
	if workers < 2 {
		workers = 2
	}
	for i := 0; i < workers; i++ {
		workersWG.Add(1)
		go func() {
			defer workersWG.Done()
			for {
				dir, ok := queue.pop()
				if !ok {
					return
				}
				var res dirResult
				var childDirs []string
				dirents, err := os.ReadDir(dir)
				if err != nil {
					res.err = fmt.Sprintf("%s: %v", dir, err)
				}
				for _, de := range dirents {
					full := filepath.Join(dir, de.Name())
					d := depthOf(full)
					if maxDepth > 0 && d >= maxDepth {
						res.entries = append(res.entries, fileInfo{path: full, isDir: de.IsDir(), depth: d})
						continue
					}
					if de.IsDir() {
						if de.Type()&fs.ModeSymlink != 0 {
							continue // 符号链接目录跳过（防环）
						}
						res.entries = append(res.entries, fileInfo{path: full, isDir: true, depth: d})
						childDirs = append(childDirs, full)
					} else if info, err := de.Info(); err == nil {
						res.entries = append(res.entries, fileInfo{path: full, size: info.Size(), depth: d})
					}
				}
				results <- res
				// 先发结果、后入队子目录、最后递减 pending：子目录入队前先 Add(n)，
				// pending 归 0 时全部子目录必已 push 完，close 后无人再 push
				if n := len(childDirs); n > 0 {
					pending.Add(int64(n))
					for _, cd := range childDirs {
						queue.push(cd)
					}
				}
				if pending.Add(-1) == 0 {
					queue.close() // 最后一个完成者关队列
				}
			}
		}()
	}
	go func() { workersWG.Wait(); close(results) }()

	allEntries := make([]fileInfo, 0, 4096)
	allErrs := make([]string, 0)
	for r := range results {
		allEntries = append(allEntries, r.entries...)
		if r.err != "" {
			allErrs = append(allErrs, r.err)
		}
	}
	return allEntries, allErrs
}

// subtreeSize —— 聚合每个目录的递归子树大小（du 语义：父目录大小含全部后代文件）。
func subtreeSize(entries []fileInfo, root string) map[string]int64 {
	// 文件按路径归入全部祖先目录
	sizes := make(map[string]int64, len(entries))
	for _, e := range entries {
		if e.isDir || e.size == 0 {
			continue
		}
		dir := filepath.Dir(e.path)
		for {
			sizes[dir] += e.size
			if dir == root || dir == "." || len(dir) <= len(root) {
				break
			}
			dir = filepath.Dir(dir)
		}
	}
	return sizes
}

func humanSize(b int64) string {
	const unit = 1024
	if b < unit {
		return fmt.Sprintf("%d B", b)
	}
	div, exp := int64(unit), 0
	for n := b / unit; n >= unit; n /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %cB", float64(b)/float64(div), "KMGTPE"[exp])
}

func resolveRoot(raw string) (string, string) {
	p := strings.TrimSpace(raw)
	if p == "" {
		p = "."
	}
	if p == "~" || strings.HasPrefix(p, "~"+string(os.PathSeparator)) || strings.HasPrefix(p, "~/") {
		home, _ := os.UserHomeDir()
		p = filepath.Join(home, strings.TrimPrefix(strings.TrimPrefix(p, "~"), "/"))
	}
	abs, err := filepath.Abs(p)
	if err != nil {
		return p, fmt.Sprintf("路径解析失败: %v", err)
	}
	info, err := os.Stat(abs)
	if err != nil {
		return abs, fmt.Sprintf("不可访问: %v", err)
	}
	if !info.IsDir() {
		return abs, "不是目录"
	}
	return abs, ""
}

func init() {
	// tree：目录树概览
	fw.RegisterTool(&fw.ToolDef{
		Name: "tree",
		Description: "目录树概览（并发遍历）：max_depth 层内每目录的子项数与子树大小（du 语义），" +
			"快速看清目录结构与空间分布。",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"dir":       fw.SchemaProp("string", "目标目录（缺省 {agent_dir}；支持 ~）"),
				"max_depth": fw.SchemaProp("number", "展示深度（默认 2）"),
			},
		},
		Execute: toolTree,
	})

	// du：空间占用排行
	fw.RegisterTool(&fw.ToolDef{
		Name: "du",
		Description: "磁盘占用分析：深度 depth 内各目录子树大小排行（du 语义，并发遍历）——" +
			"「哪个目录吃掉空间」一键定位。",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"dir":    fw.SchemaProp("string", "目标目录（缺省 {agent_dir}；支持 ~）"),
				"depth":  fw.SchemaProp("number", "聚合深度（默认 1：直接子目录）"),
				"top_k":  fw.SchemaProp("number", "返回前 K 名（默认 15）"),
			},
		},
		Execute: toolDu,
	})

	// top：大文件排行
	fw.RegisterTool(&fw.ToolDef{
		Name: "top",
		Description: "大文件排行：目录树下最大的 K 个文件（并发遍历）——清理/定位大文件。",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"dir":    fw.SchemaProp("string", "目标目录（缺省 {agent_dir}；支持 ~）"),
				"top_k":  fw.SchemaProp("number", "返回前 K 名（默认 20）"),
				"suffix": fw.SchemaProp("string", "按扩展名过滤（如 .log；缺省全部）"),
			},
		},
		Execute: toolTop,
	})

	// depth：结构与深度统计
	fw.RegisterTool(&fw.ToolDef{
		Name: "depth",
		Description: "目录结构统计：文件/目录总数、总大小、最大深度与最深路径、平均文件大小、空目录数。",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"dir": fw.SchemaProp("string", "目标目录（缺省 {agent_dir}；支持 ~）"),
			},
		},
		Execute: toolDepth,
	})
}

func commonWalk(args map[string]any) (string, []fileInfo, []string, int, string) {
	dirArg := fw.StrArg(args, "dir")
	if dirArg == "" {
		dirArg = fw.AgentDir()
	}
	root, errStr := resolveRoot(dirArg)
	if errStr != "" {
		return root, nil, nil, 0, errStr
	}
	// 始终全量遍历（maxDepth 仅控制展示深度——否则子树大小/总量统计被截断失真）
	entries, errs := walkConcurrent(root, 0)
	return root, entries, errs, 0, ""
}

func toolTree(args map[string]any) fw.ToolResult {
	root, entries, errs, _, errStr := commonWalk(args)
	if errStr != "" {
		return fw.ToolErr(errStr)
	}
	maxDepth := 2
	if d, ok := args["max_depth"].(float64); ok && d > 0 {
		maxDepth = int(d)
	}
	sizes := subtreeSize(entries, root)
	// 只显示 ≤maxDepth 的目录
	type row struct {
		path  string
		depth int
		items int
		size  int64
	}
	dirs := map[string]*row{}
	childCount := map[string]int{}
	for _, e := range entries {
		if parent := filepath.Dir(e.path); parent != root {
			childCount[parent]++
		}
	}
	for _, e := range entries {
		if !e.isDir || e.depth > maxDepth {
			continue
		}
		if _, ok := dirs[e.path]; !ok {
			dirs[e.path] = &row{path: e.path, depth: e.depth}
		}
	}
	// root 自身
	lines := []string{fmt.Sprintf("%s  %s（%d 项）", humanSize(sizes[root]), root, childCount[root]+len(dirs))}
	var sorted []*row
	for _, r := range dirs {
		sorted = append(sorted, r)
	}
	sort.Slice(sorted, func(i, j int) bool {
		si, sj := sizes[sorted[i].path], sizes[sorted[j].path]
		if si != sj {
			return si > sj
		}
		return sorted[i].path < sorted[j].path
	})
	topN := sorted
	if len(topN) > 40 {
		topN = topN[:40]
		lines = append(lines, "（仅显示前 40 目录）")
	}
	for _, r := range topN {
		indent := strings.Repeat("  ", r.depth)
		rel, _ := filepath.Rel(root, r.path)
		lines = append(lines, fmt.Sprintf("%s%s  %s%s（%d 项）", indent, humanSize(sizes[r.path]), rel, string(os.PathSeparator), childCount[r.path]))
	}
	if len(errs) > 0 {
		lines = append(lines, fmt.Sprintf("（%d 个不可读子目录已跳过）", len(errs)))
	}
	return fw.ToolOk(strings.Join(lines, "\n"), map[string]any{
		"root": root, "dirs": len(dirs), "errors": len(errs),
	})
}

func toolDu(args map[string]any) fw.ToolResult {
	root, entries, errs, _, errStr := commonWalk(args)
	if errStr != "" {
		return fw.ToolErr(errStr)
	}
	depth := 1
	if d, ok := args["depth"].(float64); ok && d > 0 {
		depth = int(d)
	}
	topK := 15
	if k, ok := args["top_k"].(float64); ok && k > 0 {
		topK = int(k)
	}
	sizes := subtreeSize(entries, root)
	// depth 层内目录大小
	type row struct {
		path string
		size int64
	}
	var rows []row
	seen := map[string]bool{}
	for _, e := range entries {
		if !e.isDir || e.depth != depth {
			continue
		}
		if !seen[e.path] {
			seen[e.path] = true
			rows = append(rows, row{e.path, sizes[e.path]})
		}
	}
	sort.Slice(rows, func(i, j int) bool {
		if rows[i].size != rows[j].size {
			return rows[i].size > rows[j].size
		}
		return rows[i].path < rows[j].path
	})
	lines := []string{fmt.Sprintf("%s 占用排行（深度 %d，根 %s）：", "目录", depth, root)}
	n := rows
	if len(n) > topK {
		n = n[:topK]
	}
	for i, r := range n {
		rel, _ := filepath.Rel(root, r.path)
		lines = append(lines, fmt.Sprintf("%2d. %s  %s", i+1, humanSize(r.size), rel))
	}
	if len(rows) > topK {
		lines = append(lines, fmt.Sprintf("（共 %d 目录，显示前 %d）", len(rows), topK))
	}
	if len(errs) > 0 {
		lines = append(lines, fmt.Sprintf("（%d 个不可读子目录已跳过）", len(errs)))
	}
	return fw.ToolOk(strings.Join(lines, "\n"), map[string]any{
		"root": root, "dirs": len(rows), "total": sizes[root], "errors": len(errs),
	})
}

func toolTop(args map[string]any) fw.ToolResult {
	root, entries, errs, _, errStr := commonWalk(args)
	if errStr != "" {
		return fw.ToolErr(errStr)
	}
	topK := 20
	if k, ok := args["top_k"].(float64); ok && k > 0 {
		topK = int(k)
	}
	suffix := strings.ToLower(fw.StrArg(args, "suffix"))
	var files []fileInfo
	for _, e := range entries {
		if e.isDir {
			continue
		}
		if suffix != "" && !strings.HasSuffix(strings.ToLower(e.path), suffix) {
			continue
		}
		files = append(files, e)
	}
	sort.Slice(files, func(i, j int) bool {
		if files[i].size != files[j].size {
			return files[i].size > files[j].size
		}
		return files[i].path < files[j].path
	})
	n := files
	if len(n) > topK {
		n = n[:topK]
	}
	lines := []string{fmt.Sprintf("最大文件排行（%s，共 %d 文件%s）：", root, len(files), suffixTag(suffix))}
	var total int64
	for _, f := range n {
		total += f.size
	}
	for i, f := range n {
		rel, _ := filepath.Rel(root, f.path)
		lines = append(lines, fmt.Sprintf("%2d. %s  %s", i+1, humanSize(f.size), rel))
	}
	if len(files) > topK {
		lines = append(lines, fmt.Sprintf("（共 %d 文件，显示前 %d 合计 %s）", len(files), topK, humanSize(total)))
	}
	if len(errs) > 0 {
		lines = append(lines, fmt.Sprintf("（%d 个不可读子目录已跳过）", len(errs)))
	}
	return fw.ToolOk(strings.Join(lines, "\n"), map[string]any{
		"root": root, "files": len(files), "errors": len(errs),
	})
}

func suffixTag(s string) string {
	if s == "" {
		return ""
	}
	return "，过滤 " + s
}

func toolDepth(args map[string]any) fw.ToolResult {
	root, entries, errs, _, errStr := commonWalk(args)
	if errStr != "" {
		return fw.ToolErr(errStr)
	}
	var fileCount, dirCount, emptyDirs, maxDepth int
	var total int64
	var deepest string
	for _, e := range entries {
		if e.isDir {
			dirCount++
			if e.depth > maxDepth {
				maxDepth = e.depth
				deepest = e.path
			}
		} else {
			fileCount++
			total += e.size
		}
	}
	// 空目录：无子条目的目录
	child := map[string]int{}
	for _, e := range entries {
		child[filepath.Dir(e.path)]++
	}
	for _, e := range entries {
		if e.isDir && child[e.path] == 0 {
			emptyDirs++
		}
	}
	avg := int64(0)
	if fileCount > 0 {
		avg = total / int64(fileCount)
	}
	lines := []string{
		fmt.Sprintf("目录: %s", root),
		fmt.Sprintf("文件: %d（共 %s，均 %s）", fileCount, humanSize(total), humanSize(avg)),
		fmt.Sprintf("目录: %d（空目录 %d）", dirCount, emptyDirs),
		fmt.Sprintf("最大深度: %d 层（%s）", maxDepth, deepest),
	}
	if len(errs) > 0 {
		lines = append(lines, fmt.Sprintf("（%d 个不可读子目录已跳过）", len(errs)))
	}
	return fw.ToolOk(strings.Join(lines, "\n"), map[string]any{
		"root": root, "files": fileCount, "dirs": dirCount, "emptyDirs": emptyDirs,
		"totalBytes": total, "maxDepth": maxDepth, "deepest": deepest, "errors": len(errs),
	})
}

func main() {
	fw.Run()
}
