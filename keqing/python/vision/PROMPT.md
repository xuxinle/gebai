本地小模型视觉识别（onnxruntime 原生推理）：ocr / locate / locate_image / detect。输入一律为图片文件路径（PNG），路径经 ctx_resolve 解析（宿主发来的通常已是绝对路径）；全部只读、无会话状态。

- 模型资产复用歌白 CV 约定（资源目录 {GEBAI_HOME}/resources/，路径约定与 TS core/cv/resources.ts 同口径）：GEBAI_CV_MODELS_DIR（det.onnx/rec.onnx/dict.txt 的 PP-OCR 三件套，缺省目录 {GEBAI_HOME}/resources/models/cv/ocr/）与 GEBAI_CV_DETECT_MODEL（或 {GEBAI_HOME}/resources/models/cv/detect/ 唯一 .onnx；旧布局 {GEBAI_HOME}/models/ 作解析回退）。
- 依赖缺失时工具返回明确安装提示（vision_pip install），不抛栈。
- 返回明确结论（文字/坐标/置信度），坐标为图片像素系；输出行数有上限（OCR 200 行），超限提示用 find/region 收窄。
