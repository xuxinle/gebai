use tauri::Manager;
use tauri_plugin_opener::OpenerExt;
use tauri_plugin_shell::ShellExt;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_shell::init())
        .setup(|app| {
            let handle = app.handle().clone();
            tauri::async_runtime::spawn(async move {
                match run_server(&handle).await {
                    Ok(port) => navigate(&handle, port),
                    Err(e) => eprintln!("[gebai-desktop] sidecar failed: {}", e),
                }
            });
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

/// Spawn the gebai server sidecar, wait until it reports its port, return it.
async fn run_server(app: &tauri::AppHandle) -> Result<u16, Box<dyn std::error::Error>> {
    let (mut rx, _child) = app.shell().sidecar("gebai")?.spawn()?;
    while let Some(event) = rx.recv().await {
        match event {
            tauri_plugin_shell::process::CommandEvent::Stdout(line) => {
                let line = String::from_utf8_lossy(&line);
                eprintln!("[gebai-desktop] {}", line.trim());
                if let Some(port) = parse_port(&line) {
                    return Ok(port);
                }
            }
            tauri_plugin_shell::process::CommandEvent::Stderr(line) => {
                eprintln!("[gebai-desktop] {}", String::from_utf8_lossy(&line));
            }
            tauri_plugin_shell::process::CommandEvent::Terminated(payload) => {
                eprintln!(
                    "[gebai-desktop] sidecar terminated: code={:?} signal={:?}",
                    payload.code, payload.signal
                );
                break;
            }
            _ => {}
        }
    }
    Err("sidecar exited without reporting a port".into())
}

/// Extract the listening port from `[gebai] listening on http://127.0.0.1:PORT ...`
fn parse_port(line: &str) -> Option<u16> {
    let start = line.find("127.0.0.1:")?;
    let rest = &line[start + "127.0.0.1:".len()..];
    let end = rest
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(rest.len());
    rest[..end].parse().ok()
}

fn navigate(app: &tauri::AppHandle, port: u16) {
    if let Some(win) = app.get_webview_window("main") {
        let url = format!("http://127.0.0.1:{}", port);
        let _ = win.eval(&format!("window.location.href = '{}'", url));
    }
}
