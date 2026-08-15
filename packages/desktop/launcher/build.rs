fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").unwrap() != "windows" {
        return;
    }
    let manifest = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
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
        .set("ProductName", "歌白")
        .compile()
        .unwrap();
}
