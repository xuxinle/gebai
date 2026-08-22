// GEBAI 原生 WebView 启动器：内嵌 gebai 服务端二进制（bun --compile 产物），
// 启动时物化到用户数据目录并 spawn（GEBAI_NO_OPEN=1 阻止其开浏览器），
// 从 stdout 解析监听端口后让 WebView 导航到 http://127.0.0.1:{port}。
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::{
    error::Error,
    fs,
    io::{BufRead, BufReader},
    path::PathBuf,
    process::{Child, Command, Stdio},
    thread,
};

use tao::{
    dpi::LogicalSize,
    event::{Event, WindowEvent},
    event_loop::{ControlFlow, EventLoopBuilder, EventLoopProxy},
    window::WindowBuilder,
};
use wry::{WebContext, WebViewBuilder};

/// 内嵌的服务端二进制（Windows 形态；路径由 build.rs 注入——完整构建 = dist/gebai.exe，
/// 场景变体构建（如 build:tutor）经 GEBAI_LAUNCHER_SERVER_EXE 指向对应产物）。
#[cfg(windows)]
const SERVER_EXE: &[u8] = include_bytes!(env!("GEBAI_EMBED_SERVER_EXE"));

/// 32px 窗口图标原始 RGBA（scripts/gen-icon.ts 生成；tao 任务栏/标题栏图标用）。
const ICON32_RGBA: &[u8] = include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/../icons/icon32.rgba"));

/// 应用标识（build.rs 注入，缺省 "gebai"）：物化数据目录名 / 非 Windows 同目录服务端文件名 / WebView 配置目录。
const APP_NAME: &str = env!("GEBAI_APP_NAME");
/// 窗口标题（build.rs 注入，缺省 "歌白"）。
const APP_TITLE: &str = env!("GEBAI_APP_TITLE");
/// 场景变体固定端口（build.rs 可选注入）：spawn 服务端时设 GEBAI_PORT——变体与完整桌面端
/// 各占独立端口，互不冲突且 localStorage origin 各自稳定；缺省不指定（服务端默认 47896）。
const APP_PORT: Option<&str> = option_env!("GEBAI_APP_PORT");

fn loading_html() -> String {
    format!("<html><body style=\"background:#1c1e22;color:#9aa0a6;font-family:system-ui;display:flex;align-items:center;justify-content:center;height:100vh;margin:0\"><div>{APP_TITLE} starting...</div></body></html>")
}

enum LauncherEvent {
    ServerReady(u16),
    ServerFailed(String),
}

fn main() -> Result<(), Box<dyn Error>> {
    let data_dir = dirs::data_local_dir()
        .or_else(dirs::data_dir)
        .ok_or("cannot locate user data directory")?
        .join(APP_NAME);
    fs::create_dir_all(&data_dir)?;

    let server_exe = materialize_server(&data_dir)?;

    let event_loop = EventLoopBuilder::<LauncherEvent>::with_user_event().build();
    let proxy = event_loop.create_proxy();
    let mut child = spawn_server(&server_exe, proxy)?;

    let icon = tao::window::Icon::from_rgba(ICON32_RGBA.to_vec(), 32, 32)?;
    let window = WindowBuilder::new()
        .with_title(APP_TITLE)
        .with_window_icon(Some(icon))
        .with_inner_size(LogicalSize::new(1280.0, 800.0))
        .with_min_inner_size(LogicalSize::new(480.0, 600.0))
        .build(&event_loop)?;

    let mut web_context = WebContext::new(Some(data_dir.join("webview")));
    let webview = WebViewBuilder::new_with_web_context(&mut web_context)
        .with_html(&loading_html())
        .build(&window)?;

    let mut url: Option<String> = None;
    event_loop.run(move |event, _, control_flow| {
        *control_flow = ControlFlow::Wait;
        match event {
            Event::UserEvent(LauncherEvent::ServerReady(port)) => {
                if url.is_none() {
                    url = Some(format!("http://127.0.0.1:{port}"));
                    let _ = webview.evaluate_script(&format!(
                        "window.location.replace('http://127.0.0.1:{port}')"
                    ));
                }
            }
            Event::UserEvent(LauncherEvent::ServerFailed(msg)) => {
                if url.is_none() {
                    url = Some(String::new());
                    let _ = webview.evaluate_script(&format!(
                        "document.body.innerText={:?}",
                        format!("gebai server failed: {msg}")
                    ));
                }
            }
            Event::WindowEvent {
                event: WindowEvent::CloseRequested,
                ..
            } => {
                let _ = child.kill();
                *control_flow = ControlFlow::Exit;
            }
            Event::LoopDestroyed => {
                let _ = child.kill();
            }
            _ => {}
        }
    })
}

/// 物化内嵌服务端二进制到用户数据目录（内容哈希不一致即覆盖；被占用时回退既有文件）。
fn materialize_server(data_dir: &PathBuf) -> Result<PathBuf, Box<dyn Error>> {
    #[cfg(windows)]
    {
        let dest = data_dir.join("gebai-server.exe");
        let mismatch = match fs::File::open(&dest) {
            Ok(mut f) => {
                // 长度先行短路（绝大多数情况下不同构建长度即不同）
                let len = f.metadata()?.len();
                if len != SERVER_EXE.len() as u64 {
                    true
                } else {
                    hash_stream(&mut f)? != hash_bytes(SERVER_EXE)
                }
            }
            Err(_) => true,
        };
        if mismatch {
            if let Err(e) = fs::write(&dest, SERVER_EXE) {
                if !dest.exists() {
                    return Err(e.into());
                }
            }
        }
        Ok(dest)
    }
    #[cfg(not(windows))]
    {
        // 非 Windows 不内嵌：使用启动器同目录的服务端二进制（文件名 = 应用标识）
        let _ = data_dir;
        let sibling = std::env::current_exe()?.with_file_name(APP_NAME);
        if !sibling.exists() {
            return Err(format!("{APP_NAME} server binary not found next to launcher").into());
        }
        Ok(sibling)
    }
}

/// 流式哈希（分块读取，覆盖大文件不整载内存）。
fn hash_stream(f: &mut fs::File) -> Result<u64, Box<dyn Error>> {
    use std::{collections::hash_map::DefaultHasher, hash::Hasher, io::Read};
    let mut hasher = DefaultHasher::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hasher.write(&buf[..n]);
    }
    Ok(hasher.finish())
}

fn hash_bytes(bytes: &[u8]) -> u64 {
    use std::{collections::hash_map::DefaultHasher, hash::Hasher};
    let mut hasher = DefaultHasher::new();
    hasher.write(bytes);
    hasher.finish()
}

/// spawn 服务端侧车：stdout 逐行解析端口（`[gebai] listening on http://HOST:PORT`），
/// 经 EventLoopProxy 通知主线程；stderr 与余下 stdout 持续排空防管道写满阻塞。
fn spawn_server(
    exe: &PathBuf,
    ready: EventLoopProxy<LauncherEvent>,
) -> Result<Child, Box<dyn Error>> {
    let mut cmd = Command::new(exe);
    cmd.env("GEBAI_NO_OPEN", "1");
    if let Some(port) = APP_PORT {
        cmd.env("GEBAI_PORT", port);
    }
    cmd.current_dir(exe.parent().unwrap_or(std::path::Path::new(".")))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    let mut child = cmd.spawn()?;
    let stdout = child.stdout.take().ok_or("no stdout pipe")?;
    let stderr = child.stderr.take().ok_or("no stderr pipe")?;
    thread::spawn(move || {
        for line in BufReader::new(stderr).lines() {
            if line.is_err() {
                break;
            }
        }
    });
    thread::spawn(move || {
        let mut reported = false;
        for line in BufReader::new(stdout).lines() {
            let Ok(line) = line else { break };
            if !reported {
                if let Some(port) = parse_port(&line) {
                    reported = true;
                    let _ = ready.send_event(LauncherEvent::ServerReady(port));
                }
            }
        }
        if !reported {
            let _ = ready.send_event(LauncherEvent::ServerFailed(
                "exited without reporting a port".into(),
            ));
        }
    });
    Ok(child)
}

/// 从服务端启动日志提取监听端口：`[gebai] listening on http://127.0.0.1:3000 (...)`。
fn parse_port(line: &str) -> Option<u16> {
    const MARKER: &str = "listening on http://";
    let rest = &line[line.find(MARKER)? + MARKER.len()..];
    let end = rest
        .find(|c: char| c.is_whitespace() || c == '/')
        .unwrap_or(rest.len());
    rest[..end].rsplit(':').next()?.parse().ok()
}
