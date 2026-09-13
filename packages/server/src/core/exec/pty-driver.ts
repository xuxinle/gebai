/**
 * 文件工作台 · 终端 PTY 驱动（Windows ConPTY）。
 *
 * 为什么是「编译成独立 exe」：Windows 的伪控制台只能经原生 API 创建（`CreatePseudoConsole`
 * + `CreateProcess` 的 `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` 属性），而服务端运行时（Bun）
 * 无原生绑定。本模块把一段自包含 C#（P/Invoke kernel32）用**系统自带**的 .NET Framework
 * 编译器（`csc.exe`）编译成小 exe 后 spawn：零第三方依赖、零编译产物分发——源码与产物都落在
 * 系统临时目录（按内容哈希命名，改内容自动换文件），运行期只付一次编译成本（组合根会预热）。
 *
 * 为什么不用 PowerShell 承载同一段 C#：宿主会消费子进程 stdin/stdout 并做编码转换，
 * 协议通道（JSON 行）与 PTY 数据会被污染；独立 exe 的两个流完全归驱动所有。
 *
 * 协议（stdin/stdout 均为行分隔 JSON，驱动内自解析——不引 JSON 库）：
 *   宿主 → 驱动：`{"t":"open","shell":"<命令行>","cwd":"<绝对路径>","cols":N,"rows":M}`（首行，必发）
 *                `{"t":"in","d":"<base64 原始字节>"}` · `{"t":"resize","cols":N,"rows":M}` · `{"t":"close"}`
 *   驱动 → 宿主：`{"t":"ready","pid":N}` · `{"t":"out","d":"<base64 原始字节>"}` ·
 *                `{"t":"exit","code":N}` · `{"t":"error","m":"..."}`
 *
 * 输出走 base64 而非裸字节：stdin/stdout 同时承载协议与数据，文本行协议最稳妥；输出按 16KB
 * 分块（xterm 自身处理跨块转义序列）。伪控制台创建有两个**必须**做对的点（否则子进程会绕过
 * 伪控制台、直接写宿主 stdout，表现为输出明文泄漏且输入不达）：`STARTF_USESTDHANDLES`
 * 且三个标准句柄置空；`bInheritHandles` 必须为 false。
 */
import { spawnSync } from "node:child_process"
import { createHash } from "node:crypto"
import { existsSync, mkdirSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"

/** ConPTY 驱动 C# 源（含入口 Main；编译为 exe 后由服务端 spawn）。 */
export const PTY_DRIVER_CS = String.raw`using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class GebaiPty
{
    [StructLayout(LayoutKind.Sequential)] public struct COORD { public short X; public short Y; }
    [StructLayout(LayoutKind.Sequential)] public struct SECURITY_ATTRIBUTES { public int nLength; public IntPtr lpSecurityDescriptor; public int bInheritHandle; }
    [StructLayout(LayoutKind.Sequential)] public struct STARTUPINFO
    {
        public int cb; public IntPtr lpReserved; public IntPtr lpDesktop; public IntPtr lpTitle;
        public int dwX; public int dwY; public int dwXSize; public int dwYSize; public int dwXCountChars; public int dwYCountChars;
        public int dwFillAttribute; public int dwFlags; public short wShowWindow; public short cbReserved2; public IntPtr lpReserved2;
        public IntPtr hStdInput; public IntPtr hStdOutput; public IntPtr hStdError;
    }
    [StructLayout(LayoutKind.Sequential)] public struct STARTUPINFOEX { public STARTUPINFO StartupInfo; public IntPtr lpAttributeList; }
    [StructLayout(LayoutKind.Sequential)] public struct PROCESS_INFORMATION { public IntPtr hProcess; public IntPtr hThread; public int dwProcessId; public int dwThreadId; }

    [DllImport("kernel32.dll", SetLastError=true)] static extern int CreatePseudoConsole(COORD size, IntPtr hInput, IntPtr hOutput, uint flags, out IntPtr phPC);
    [DllImport("kernel32.dll", SetLastError=true)] static extern void ClosePseudoConsole(IntPtr hPC);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool ResizePseudoConsole(IntPtr hPC, COORD size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool CreatePipe(out IntPtr hRead, out IntPtr hWrite, ref SECURITY_ATTRIBUTES sa, uint size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, int flags, ref IntPtr size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attr, IntPtr value, IntPtr size, IntPtr prev, IntPtr ret);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string cwd, ref STARTUPINFOEX si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetHandleInformation(IntPtr h, uint mask, uint flags);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool ReadFile(IntPtr h, IntPtr buf, uint n, out uint read, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool WriteFile(IntPtr h, byte[] buf, uint n, out uint written, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr h, out uint code);

    // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE：lpValue 必须直接传 HPCON 句柄本身
    // （传「指向句柄的指针」会让子进程初始化失败，退出码 0xC0000142）。
    const ulong PSEUDOCONSOLE_ATTR = 0x00020016;
    const uint EXTENDED_STARTUPINFO_PRESENT = 0x00080000;
    const int STARTF_USESTDHANDLES = 0x00000100;

    static IntPtr hOutRead = IntPtr.Zero;
    static IntPtr hInWrite = IntPtr.Zero;
    static IntPtr hPC = IntPtr.Zero;
    static IntPtr hProcess = IntPtr.Zero;
    static readonly object outLock = new object();

    public static int ChildPid = 0;

    /** 创建伪控制台并拉起子进程；返回空串表示成功，否则为错误描述。 */
    public static string Start(string shell, string cwd, short cols, short rows)
    {
        var sa = new SECURITY_ATTRIBUTES();
        sa.nLength = Marshal.SizeOf(typeof(SECURITY_ATTRIBUTES));
        sa.bInheritHandle = 1;
        IntPtr inRead, inWrite, outRead, outWrite;
        if (!CreatePipe(out inRead, out inWrite, ref sa, 0)) return "CreatePipe(in) failed: " + Marshal.GetLastWin32Error();
        if (!CreatePipe(out outRead, out outWrite, ref sa, 0)) return "CreatePipe(out) failed: " + Marshal.GetLastWin32Error();
        // 宿主自用的一端不可继承（否则子进程持有一份副本，管道 EOF 语义被破坏）
        SetHandleInformation(inWrite, 1, 0);
        SetHandleInformation(outRead, 1, 0);
        hOutRead = outRead;
        hInWrite = inWrite;

        var size = new COORD();
        size.X = cols;
        size.Y = rows;
        IntPtr pc;
        int hr = CreatePseudoConsole(size, inRead, outWrite, 0, out pc);
        if (hr != 0) return "CreatePseudoConsole failed: hr=" + hr;
        hPC = pc;
        CloseHandle(inRead);
        CloseHandle(outWrite);

        IntPtr listSize = IntPtr.Zero;
        InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref listSize);
        IntPtr list = Marshal.AllocHGlobal(listSize);
        if (!InitializeProcThreadAttributeList(list, 1, 0, ref listSize)) return "InitializeProcThreadAttributeList failed: " + Marshal.GetLastWin32Error();
        if (!UpdateProcThreadAttribute(list, 0, (IntPtr)PSEUDOCONSOLE_ATTR, pc, (IntPtr)IntPtr.Size, IntPtr.Zero, IntPtr.Zero)) return "UpdateProcThreadAttribute failed: " + Marshal.GetLastWin32Error();

        var si = new STARTUPINFOEX();
        si.StartupInfo.cb = Marshal.SizeOf(typeof(STARTUPINFOEX));
        // 子进程标准句柄显式置空：不声明 STDHANDLES 的话，子进程会继承宿主 stdout，
        // 绕过伪控制台直接写宿主管道（输出明文泄漏 + 输入不达）；bInheritHandles 也必须为 false。
        si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = IntPtr.Zero;
        si.StartupInfo.hStdOutput = IntPtr.Zero;
        si.StartupInfo.hStdError = IntPtr.Zero;
        si.lpAttributeList = list;
        var pi = new PROCESS_INFORMATION();
        if (!CreateProcess(null, shell, IntPtr.Zero, IntPtr.Zero, false, EXTENDED_STARTUPINFO_PRESENT, IntPtr.Zero, cwd, ref si, out pi)) return "CreateProcess failed: " + Marshal.GetLastWin32Error();
        hProcess = pi.hProcess;
        ChildPid = pi.dwProcessId;
        Marshal.FreeHGlobal(list);
        CloseHandle(pi.hThread);
        return "";
    }

    public static void WriteBytes(byte[] data)
    {
        if (hInWrite == IntPtr.Zero || data.Length == 0) return;
        uint written;
        WriteFile(hInWrite, data, (uint)data.Length, out written, IntPtr.Zero);
    }

    public static bool Resize(short cols, short rows)
    {
        if (hPC == IntPtr.Zero) return false;
        var size = new COORD();
        size.X = cols;
        size.Y = rows;
        return ResizePseudoConsole(hPC, size);
    }

    public static void ClosePty()
    {
        if (hPC != IntPtr.Zero) { ClosePseudoConsole(hPC); hPC = IntPtr.Zero; }
        if (hInWrite != IntPtr.Zero) { CloseHandle(hInWrite); hInWrite = IntPtr.Zero; }
        if (hOutRead != IntPtr.Zero) { CloseHandle(hOutRead); hOutRead = IntPtr.Zero; }
    }

    /** 单行 JSON 写出（stdout 同时是协议通道，独占锁 + 立即 flush）。 */
    static void Emit(string json)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(json + "\n");
        lock (outLock)
        {
            var stdout = Console.OpenStandardOutput();
            stdout.Write(bytes, 0, bytes.Length);
            stdout.Flush();
        }
    }

    static string Esc(string s)
    {
        return s.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n");
    }

    /** 扁平 JSON 取值（协议只有一层对象；不引 JSON 库）。 */
    static string Val(string s, string key)
    {
        if (s == null) return null;
        string pat = "\"" + key + "\":";
        int i = s.IndexOf(pat);
        if (i < 0) return null;
        i += pat.Length;
        if (i >= s.Length) return null;
        if (s[i] == '"')
        {
            var sb = new StringBuilder();
            i++;
            while (i < s.Length)
            {
                char ch = s[i];
                if (ch == '"') break;
                if (ch == '\\' && i + 1 < s.Length)
                {
                    char nx = s[i + 1];
                    if (nx == '"') sb.Append('"');
                    else if (nx == '\\') sb.Append('\\');
                    else if (nx == 'n') sb.Append('\n');
                    else if (nx == 'r') sb.Append('\r');
                    else sb.Append(nx);
                    i += 2;
                    continue;
                }
                sb.Append(ch);
                i++;
            }
            return sb.ToString();
        }
        int j = i;
        while (j < s.Length && s[j] != ',' && s[j] != '}') j++;
        return s.Substring(i, j - i).Trim();
    }

    static int IntVal(string s, string key, int fallback)
    {
        string v = Val(s, key);
        if (v == null) return fallback;
        int n;
        return int.TryParse(v, out n) ? n : fallback;
    }

    /** 输出泵：阻塞读伪控制台输出并逐块推给宿主；读取失败/EOF 即子进程收尾。 */
    static void Pump()
    {
        IntPtr buf = Marshal.AllocHGlobal(16384);
        try
        {
            while (true)
            {
                uint n;
                if (!ReadFile(hOutRead, buf, 16384, out n, IntPtr.Zero)) break;
                if (n == 0) break;
                byte[] chunk = new byte[n];
                Marshal.Copy(buf, chunk, 0, (int)n);
                Emit("{\"t\":\"out\",\"d\":\"" + Convert.ToBase64String(chunk) + "\"}");
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buf);
        }
        uint code = 0;
        if (hProcess != IntPtr.Zero) GetExitCodeProcess(hProcess, out code);
        Emit("{\"t\":\"exit\",\"code\":" + code + "}");
        Environment.Exit(0);
    }

    /** 主循环：首行 open，其后 in / resize / close。 */
    public static void Run()
    {
        string first = Console.In.ReadLine();
        if (first == null) return;
        string shell = Val(first, "shell");
        string cwd = Val(first, "cwd");
        int cols = IntVal(first, "cols", 120);
        int rows = IntVal(first, "rows", 30);
        if (shell == null || shell.Length == 0)
        {
            Emit("{\"t\":\"error\",\"m\":\"missing shell\"}");
            return;
        }
        string err = Start(shell, cwd, (short)cols, (short)rows);
        if (err.Length != 0)
        {
            Emit("{\"t\":\"error\",\"m\":\"" + Esc(err) + "\"}");
            return;
        }
        Emit("{\"t\":\"ready\",\"pid\":" + ChildPid + "}");
        var pump = new Thread(Pump);
        pump.IsBackground = true;
        pump.Start();
        while (true)
        {
            string line = Console.In.ReadLine();
            if (line == null) break;
            string type = Val(line, "t");
            if (type == "in")
            {
                string d = Val(line, "d");
                if (d != null && d.Length != 0)
                {
                    try { WriteBytes(Convert.FromBase64String(d)); } catch (Exception) { }
                }
            }
            else if (type == "resize")
            {
                int c = IntVal(line, "cols", 0);
                int r = IntVal(line, "rows", 0);
                if (c > 0 && r > 0) Resize((short)c, (short)r);
            }
            else if (type == "close")
            {
                ClosePty();
                break;
            }
        }
    }

    public static int Main(string[] args)
    {
        Run();
        return 0;
    }
}
`

/** 驱动启动方式：可用的 spawn 命令行，或不可用原因（供上层降级）。 */
export interface PtyDriverLaunch {
  ok: boolean
  /** ok=true：直接 spawn 该命令行（argv 形式，无需 shell 引号处理）。 */
  cmd?: string[]
  /** ok=false：面向用户的中文原因。 */
  reason?: string
}

/** .NET Framework 自带编译器（按平台位宽优先 x64）。 */
function findCsc(): string | null {
  const windir = process.env.WINDIR || "C:\\Windows"
  const candidates = [
    join(windir, "Microsoft.NET", "Framework64", "v4.0.30319", "csc.exe"),
    join(windir, "Microsoft.NET", "Framework", "v4.0.30319", "csc.exe"),
  ]
  for (const p of candidates) if (existsSync(p)) return p
  return null
}

/** 编译结果缓存（成功才缓存：失败允许下次重试，例如临时的杀软拦截）。 */
let cachedLaunch: PtyDriverLaunch | null = null

/**
 * 准备 PTY 驱动（首次调用同步编译，约 1s；组合根在启动后预热以避开用户首点等待）。
 * 非 Windows / 无 csc / 编译失败时返回 ok:false，由上层降级到管道式终端。
 */
export function preparePtyDriver(): PtyDriverLaunch {
  if (cachedLaunch?.ok) return cachedLaunch
  if (process.platform !== "win32") return { ok: false, reason: "PTY 终端仅支持 Windows（当前平台降级为管道式终端）" }
  const csc = findCsc()
  if (!csc) return { ok: false, reason: "未找到 .NET Framework 编译器 csc.exe（降级为管道式终端）" }
  const dir = join(tmpdir(), "gebai-pty")
  const hash = createHash("sha256").update(PTY_DRIVER_CS).digest("hex").slice(0, 16)
  const src = join(dir, `driver-${hash}.cs`)
  const exe = join(dir, `driver-${hash}.exe`)
  try {
    mkdirSync(dir, { recursive: true })
    if (!existsSync(exe)) {
      // 带 BOM 写入：csc 默认按系统代码页读源文件，源码含中文注释，BOM 保证按 UTF-8 解析
      writeFileSync(src, `\uFEFF${PTY_DRIVER_CS}`, "utf8")
      const r = spawnSync(csc, ["/nologo", "/target:exe", "/platform:x64", `/out:${exe}`, src], { timeout: 60_000 })
      if (r.status !== 0 || !existsSync(exe)) {
        const detail = (r.stderr?.toString() || r.error?.message || "").trim().slice(0, 200)
        return { ok: false, reason: `PTY 驱动编译失败${detail ? `：${detail}` : ""}（降级为管道式终端）` }
      }
    }
    cachedLaunch = { ok: true, cmd: [exe] }
    return cachedLaunch
  } catch (err) {
    return { ok: false, reason: `PTY 驱动准备失败：${(err as Error).message}（降级为管道式终端）` }
  }
}
