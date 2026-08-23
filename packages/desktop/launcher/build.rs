use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));

    // —— 场景变体参数（构建期环境变量注入，main.rs 经 rustc-env 消费；缺省值 = 完整桌面端，行为不变）——
    // 内嵌服务端二进制（Windows include_bytes；缺省 dist/gebai.exe，变体构建指向对应产物）
    let desktop_dir = manifest.parent().unwrap().to_path_buf();
    let server_exe = std::env::var("GEBAI_LAUNCHER_SERVER_EXE")
        .map(PathBuf::from)
        .unwrap_or_else(|_| desktop_dir.join("dist").join("gebai.exe"));
    println!("cargo:rustc-env=GEBAI_EMBED_SERVER_EXE={}", server_exe.display());
    println!("cargo:rerun-if-env-changed=GEBAI_LAUNCHER_SERVER_EXE");
    if server_exe.is_absolute() {
        println!("cargo:rerun-if-changed={}", server_exe.display());
    }
    // 应用标识（物化数据目录名 / 非 Windows 同目录服务端文件名 / WebView 配置目录）与窗口标题
    let app_name = std::env::var("GEBAI_LAUNCHER_APP_NAME").unwrap_or_else(|_| "gebai".to_string());
    println!("cargo:rustc-env=GEBAI_APP_NAME={app_name}");
    println!("cargo:rerun-if-env-changed=GEBAI_LAUNCHER_APP_NAME");
    let app_title = std::env::var("GEBAI_LAUNCHER_TITLE").unwrap_or_else(|_| "歌白".to_string());
    println!("cargo:rustc-env=GEBAI_APP_TITLE={app_title}");
    println!("cargo:rerun-if-env-changed=GEBAI_LAUNCHER_TITLE");
    // 可选固定端口：变体用独立端口避免与完整桌面端（47896）冲突，并保持 localStorage origin 稳定
    if let Ok(port) = std::env::var("GEBAI_LAUNCHER_PORT") {
        println!("cargo:rustc-env=GEBAI_APP_PORT={port}");
        println!("cargo:rerun-if-env-changed=GEBAI_LAUNCHER_PORT");
    }

    if std::env::var("CARGO_CFG_TARGET_OS").unwrap() != "windows" {
        return;
    }
    // 窗口图标原始 RGBA（main.rs include_bytes）：缺失时给出可操作的报错
    let rgba = manifest.join("../icons/icon32.rgba");
    if !rgba.exists() {
        panic!("icons/icon32.rgba 缺失：请先运行 bun run --cwd packages/desktop scripts/gen-icon.ts（或完整 server:build）");
    }
    println!("cargo:rerun-if-changed={}", rgba.display());
    // exe 资源图标（Explorer/文件管理器）
    let icon = manifest.join("../icons/icon.ico");
    if !icon.exists() {
        panic!("icons/icon.ico 缺失：请先运行 bun run --cwd packages/desktop scripts/gen-icon.ts（或完整 server:build）");
    }
    // 图标在包目录之外，须显式声明：ICO 再生成时重新嵌入
    println!("cargo:rerun-if-changed={}", icon.display());
    winresource::WindowsResource::new()
        .set_icon_with_id(&icon.to_string_lossy(), "1")
        .set("ProductName", &app_title)
        .compile()
        .unwrap();
}
