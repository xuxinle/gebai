"""vision 子代理专属工具（tools.py 合并模式，由 keqing/python/driver.py 加载）：
本地小模型视觉识别四工具——ocr / locate / locate_image / detect。

onnxruntime 原生推理 + numpy 前后处理（移植自 TS core/cv 的纯函数实现，算法约定对齐：
det 归一化 ImageNet mean/std、rec 归一化 0.5/0.5、CTC blank=0、DB unclip、letterbox 114 灰、
v8/v5 双输出形态、ultralytics ONNX 元数据自适应）；模板匹配为 numpy 向量化重写（零均值 NCC +
积分图 + 粗扫多相位 + 全分辨率精化，与 TS template.ts 同算法）。

模型资产复用歌白 CV 约定：
- OCR：GEBAI_CV_MODELS_DIR → {GEBAI_HOME}/models/ocr → {GEBAI_HOME}/vendor/cv-models
  → 源码形态 packages/server/assets/cv-models（det.onnx / rec.onnx / dict.txt 三件套）
- 检测：GEBAI_CV_DETECT_MODEL（ultralytics 导出自动读 imgsz/names）或
  {GEBAI_HOME}/models/detect/ 唯一 .onnx
图片路径一律经 driver.ctx_resolve 解析（相对路径基准=请求级会话工作区）。
"""

import math
import os

AGENT_NAME = "vision"

# ---------------- 依赖探测（缺依赖不阻断工具集：探测延迟到调用时，给明确安装提示） ----------------

_PIP_HINT = "依赖缺失：请运行 vision_pip（action=install，packages=\"onnxruntime numpy pillow\"）安装视觉识别依赖后重试"


def _deps_missing():
    """探测三方依赖（numpy/onnxruntime/PIL）：缺返回错误文本，齐备返回 None。
    延迟到调用时探测——模块加载阶段缺依赖不阻断工具集上报（driver 加载失败会回退基础工具
    并丢弃 ocr 等工具，模型将看不到安装指引）。"""
    try:
        import numpy  # noqa: F401
        import onnxruntime  # noqa: F401
        from PIL import Image  # noqa: F401
        return None
    except BaseException as e:  # noqa: BLE001
        return f"{_PIP_HINT}（{e}）"


def _need_deps():
    err = _deps_missing()
    if err:
        raise RuntimeError(err)


# ---------------- 模型资产解析 ----------------


def gebai_home():
    return os.environ.get("GEBAI_HOME") or os.path.expanduser("~/.gebai")


def _src_assets_dir():
    """源码形态模型目录（driver.py 位于 keqing/python/ → 仓库根 assets）：
    packages/server/assets/cv-models（build-cv-embed.ts 下载产物）。"""
    lang_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for _ in range(4):
        cand = os.path.join(lang_dir, "packages", "server", "assets", "cv-models")
        if os.path.isfile(os.path.join(cand, "det.onnx")):
            return cand
        lang_dir = os.path.dirname(lang_dir)
    return None


def resolve_ocr_dir():
    """OCR 三件套目录：GEBAI_CV_MODELS_DIR → {GEBAI_HOME}/models/ocr → vendor → 源码形态。"""
    import driver

    cands = []
    env_dir = driver.ctx_env("GEBAI_CV_MODELS_DIR")
    if env_dir:
        cands.append(env_dir)
    home = gebai_home()
    cands.append(os.path.join(home, "models", "ocr"))
    cands.append(os.path.join(home, "vendor", "cv-models"))
    src = _src_assets_dir()
    if src:
        cands.append(src)
    for d in cands:
        if os.path.isfile(os.path.join(d, "det.onnx")):
            return d
    return None


def resolve_detect_model():
    """检测模型：GEBAI_CV_DETECT_MODEL → {GEBAI_HOME}/models/detect/ 唯一 .onnx。"""
    import driver

    explicit = driver.ctx_env("GEBAI_CV_DETECT_MODEL")
    if explicit and os.path.isfile(explicit):
        return explicit, None
    d = os.path.join(gebai_home(), "models", "detect")
    try:
        onnx = [n for n in os.listdir(d) if n.lower().endswith(".onnx")]
    except OSError:
        return None, "目标检测未配置：设置 GEBAI_CV_DETECT_MODEL 或把唯一 .onnx 放入 {GEBAI_HOME}/models/detect/（ultralytics 导出自动读 imgsz/names；其他来源需 GEBAI_CV_DETECT_LABELS）"
    if len(onnx) == 1:
        return os.path.join(d, onnx[0]), None
    if not onnx:
        return None, "目标检测未配置：{GEBAI_HOME}/models/detect/ 无 .onnx（自备 ultralytics YOLO 导出模型放入即用）"
    return None, f"目标检测模型歧义：{d} 下有 {len(onnx)} 个 .onnx，请用 GEBAI_CV_DETECT_MODEL 显式指定"


# ---------------- 推理会话（进程级缓存——模型加载一次全会话复用） ----------------

_sessions = {}


def _session(path, input_name=None):
    import onnxruntime as ort

    key = f"{path}:{input_name or ''}"
    if key not in _sessions:
        so = ort.SessionOptions()
        so.intra_op_num_threads = max(1, (os.cpu_count() or 2))
        _sessions[key] = ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])
    return _sessions[key]


# ---------------- 图片加载 ----------------


def load_image(path_str):
    """加载图片为 (H, W, 3) uint8 RGB；路径经 ctx_resolve（相对=会话工作区）。"""
    from PIL import Image
    import driver

    p = driver.ctx_resolve(str(path_str or ""))
    if not os.path.isfile(p):
        raise RuntimeError(f"图片不存在: {p}（相对路径基准=会话工作区；本工具无缺省图像源，需要现截屏幕/页面用 desktop/playwright 子代理）")
    try:
        with Image.open(p) as im:
            img = im.convert("RGB")
    except BaseException as e:  # noqa: BLE001
        raise RuntimeError(f"图片读取/解码失败（{p}）: {e}")
    return img


def parse_region(s):
    """'x,y,w,h' → (x, y, w, h)；非法返回 None。"""
    try:
        parts = [float(v) for v in str(s).replace("，", ",").split(",")]
        if len(parts) != 4:
            return None
        return parts
    except ValueError:
        return None


def crop_arr(arr, region):
    """numpy HWC 数组按 region 裁剪（越界钳制），返回 (裁剪数组, offX, offY)。"""
    x, y, w, h = [max(0, int(round(v))) for v in region]
    H, W = arr.shape[:2]
    x, y = min(x, W - 1), min(y, H - 1)
    w, h = max(1, min(w, W - x)), max(1, min(h, H - y))
    return arr[y : y + h, x : x + w], x, y


# ---------------- OCR 前后处理（对齐 TS core/cv/ocr.ts） ----------------

DET_MAX_SIDE = 1920

# OCR 大图分块：整图长边超过 OCR_TILE_SIDE 即切块（每块以原生分辨率走后端模型），
# 相邻块重叠 OCR_TILE_OVERLAP 像素以免行被切在边界；重叠区同一条行按框 IoU 去重。
# 检测输入尺寸对耗时影响很小（FHD 下 960→1920 仅 +20% 耗时）而准确率差异巨大
# （小字高分命 2/7→7/7），故默认以原生尺度送检。
OCR_TILE_SIDE = 2560
OCR_TILE_OVERLAP = 320
OCR_MERGE_IOU = 0.5
DB_THRESHOLD = 0.3
DB_BOX_THRESHOLD = 0.5
DB_UNCLIP_RATIO = 1.6
REC_HEIGHT = 48
REC_WIDTH = 320
CTC_BLANK_INDEX = 0


def det_preprocess(arr, max_side=DET_MAX_SIDE):
    """等比缩放（长边 ≤ max_side）+ 右/下零填充至 32 倍数；返回 (NCHW float32, pw, ph, scale)。"""
    from PIL import Image
    import numpy as np

    DET_MEAN = np.array([0.485, 0.456, 0.406], dtype=np.float32)
    DET_STD = np.array([0.229, 0.224, 0.225], dtype=np.float32)
    H, W = arr.shape[:2]
    scale = min(1.0, max_side / max(H, W))
    w, h = max(1, round(W * scale)), max(1, round(H * scale))
    pw, ph = max(32, math.ceil(w / 32) * 32), max(32, math.ceil(h / 32) * 32)
    resized = np.asarray(
        Image.fromarray(arr).resize((w, h), Image.BILINEAR), dtype=np.float32
    )
    canvas = np.zeros((ph, pw, 3), dtype=np.float32)
    canvas[:h, :w] = resized
    chw = (canvas / 255.0 - DET_MEAN) / DET_STD
    return chw.transpose(2, 0, 1)[None], pw, ph, scale


def db_postprocess(prob, map_w, map_h, src_w, src_h, scale):
    """DB 后处理：阈值→膨胀→8 连通域→外接框→unclip→阅读序（与 TS dbPostprocess 同算法）。"""
    import numpy as np

    mask = (prob > DB_THRESHOLD).astype(np.uint8)
    # 3x3 膨胀（一次）
    dil = mask.copy()
    padded = np.pad(mask, 1)
    neigh = np.zeros_like(mask, dtype=np.int32)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            neigh += padded[1 + dy : 1 + dy + map_h, 1 + dx : 1 + dx + map_w]
    dil[(mask == 0) & (neigh > 0)] = 1
    # 连通域（numpy 两轮扫描近似：本实现用 BFS——图通常 < 960x960，纯 Python 可接受；
    # 优化：按行 run 合并后再 BFS，UI 截图上毫秒级）
    visited = np.zeros((map_h, map_w), dtype=bool)
    boxes = []
    ys, xs = np.nonzero(dil)
    for start in zip(ys.tolist(), xs.tolist()):
        y0, x0 = start
        if visited[y0, x0]:
            continue
        stack = [start]
        visited[y0, x0] = True
        minx = maxx = x0
        miny = maxy = y0
        count = 0
        score_sum = 0.0
        while stack:
            y, x = stack.pop()
            if x < minx:
                minx = x
            if x > maxx:
                maxx = x
            if y < miny:
                miny = y
            if y > maxy:
                maxy = y
            if mask[y, x]:
                count += 1
                score_sum += float(prob[y, x])
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < map_h and 0 <= nx < map_w and dil[ny, nx] and not visited[ny, nx]:
                        visited[ny, nx] = True
                        stack.append((ny, nx))
        score = score_sum / count if count else 0.0
        if score < DB_BOX_THRESHOLD:
            continue
        w = maxx - minx + 1
        h = maxy - miny + 1
        if w < 3 or h < 3:
            continue
        d = (w * h * DB_UNCLIP_RATIO) / (2 * (w + h))
        x0 = min(max((minx - d) / scale, 0), src_w)
        y0 = min(max((miny - d) / scale, 0), src_h)
        x1 = min(max((maxx + 1 + d) / scale, 0), src_w)
        y1 = min(max((maxy + 1 + d) / scale, 0), src_h)
        if x1 - x0 < 3 or y1 - y0 < 3:
            continue
        boxes.append({"x": x0, "y": y0, "w": x1 - x0, "h": y1 - y0, "score": score})
    return sort_reading_order(boxes)


def sort_reading_order(boxes):
    """按垂直中心聚类成行（容差 = 0.6×中位行高），行内按 x 排序。"""
    if len(boxes) < 2:
        return list(boxes)
    heights = sorted(b["h"] for b in boxes)
    tol = max(8.0, heights[len(heights) // 2] * 0.6)
    return sorted(boxes, key=lambda b: (round((b["y"] + b["h"] / 2) / tol), b["x"]))


def rec_preprocess(crop_arr):
    """等比缩放到 48 高（宽钳制 [16, 320]）+ 右侧零填充；返回 NCHW float32。"""
    from PIL import Image
    import numpy as np

    H, W = crop_arr.shape[:2]
    w = min(REC_WIDTH, max(16, round(W / H * REC_HEIGHT))) if H else 16
    resized = np.asarray(
        Image.fromarray(crop_arr).resize((w, REC_HEIGHT), Image.BILINEAR), dtype=np.float32
    )
    canvas = np.zeros((REC_HEIGHT, REC_WIDTH, 3), dtype=np.float32)
    canvas[:, :w] = resized
    chw = (canvas / 255.0 - 0.5) / 0.5
    return chw.transpose(2, 0, 1)[None]


def ctc_decode(probs, chars):
    """CTC 贪心解码（逐时间步 argmax，合并连续相同后去 blank=0）。probs: (steps, classes)。"""
    import numpy as np

    steps, classes = probs.shape
    best = probs.argmax(axis=1)
    best_p = probs[np.arange(steps), best]
    text_parts = []
    score_sum = 0.0
    hits = 0
    prev = -1
    for t in range(steps):
        b = int(best[t])
        if b != CTC_BLANK_INDEX and b != prev and b < len(chars):
            text_parts.append(chars[b])
            score_sum += float(best_p[t])
            hits += 1
        prev = b
    return "".join(text_parts), (score_sum / hits if hits else 0.0)


# ---------------- 模板匹配（对齐 TS core/cv/template.ts，numpy 向量化） ----------------


def to_gray(arr):
    """HWC RGB → (H, W) float32 灰度（ITU-R BT.601 与 TS 实现一致）。"""
    import numpy as np

    r, g, b = arr[:, :, 0].astype(np.float32), arr[:, :, 1].astype(np.float32), arr[:, :, 2].astype(np.float32)
    return 0.299 * r + 0.587 * g + 0.114 * b


def downsample(g, s, ox=0, oy=0):
    """box 降采样（s×s 块均值，边缘块按实际像素数平均）；(ox, oy) 为采样原点相位偏移。

    与 TS downsample 同语义（同一模板在两侧得到一致分数）。
    """
    import numpy as np

    if s <= 1:
        return g if g.dtype == np.float32 else g.astype(np.float32)
    sub = g[oy:, ox:]
    if sub.size == 0:
        return np.zeros((0, 0), dtype=np.float32)
    H, W = sub.shape
    pad_h, pad_w = (-H) % s, (-W) % s
    f = sub.astype(np.float32)
    mask = np.ones_like(f, dtype=np.float32)
    if pad_h or pad_w:
        f = np.pad(f, ((0, pad_h), (0, pad_w)))
        mask = np.pad(mask, ((0, pad_h), (0, pad_w)))
    h, w = f.shape[0] // s, f.shape[1] // s
    blocks = f.reshape(h, s, w, s)
    cnt = mask.reshape(h, s, w, s).sum(axis=(1, 3))
    return (blocks.sum(axis=(1, 3)) / np.maximum(cnt, 1)).astype(np.float32)


class _Tpl:
    def __init__(self, g):
        import numpy as np

        self.data = g.astype(np.float32)
        self.h, self.w = g.shape
        self.norm = float(np.sqrt(((self.data - self.data.mean()) ** 2).sum()))


class _Integrals:
    """积分图（含平方）：窗口和与方差 O(1)。"""

    def __init__(self, g):
        import numpy as np

        f = g.astype(np.float64)
        self.s = np.zeros((g.shape[0] + 1, g.shape[1] + 1))
        self.sq = np.zeros((g.shape[0] + 1, g.shape[1] + 1))
        np.cumsum(np.cumsum(f, axis=0), axis=1, out=self.s[1:, 1:])
        np.cumsum(np.cumsum(f * f, axis=0), axis=1, out=self.sq[1:, 1:])

    def window(self, x, y, w, h):
        s = self.s[y + h, x + w] - self.s[y, x + w] - self.s[y + h, x] + self.s[y, x]
        sq = self.sq[y + h, x + w] - self.sq[y, x + w] - self.sq[y + h, x] + self.sq[y, x]
        return s, sq


def _window_sums(integ, h, w):
    """积分图上的全图滑窗和/平方和（向量化）——shape (H-h+1, W-w+1)。"""
    c1, c2 = integ.s, integ.sq
    return (
        c1[h:, w:] - c1[:-h, w:] - c1[h:, :-w] + c1[:-h, :-w],
        c2[h:, w:] - c2[:-h, w:] - c2[h:, :-w] + c2[:-h, :-w],
    )


def _ncc_at(g, integ, tpl, x, y):
    """单点零均值 NCC（与 TS ncc 同式）。"""
    n = tpl.w * tpl.h
    s, sq = integ.window(x, y, tpl.w, tpl.h)
    var = sq - s * s / n
    denom = math.sqrt(max(var, 0.0)) * tpl.norm
    if denom < 1e-6:
        return 0.0
    window = g[y : y + tpl.h, x : x + tpl.w]
    dot = float(((window - window.mean()) * (tpl.data - tpl.data.mean())).sum())
    return dot / denom


def match_template(search_arr, template_arr, threshold=0.8):
    """零均值 NCC 模板匹配：粗扫（多相位降采样）+ 全分辨率精化 + NMS，至多 5 个。
    与 TS matchTemplate 同算法；粗扫内层用 numpy 滑窗向量化（reshape trick）。"""
    import numpy as np
    from numpy.lib.stride_tricks import sliding_window_view

    g_search = to_gray(search_arr)
    g_tpl = to_gray(template_arr)
    th, tw = g_tpl.shape
    H, W = g_search.shape
    if tw > W or th > H:
        raise RuntimeError(f"模板（{tw}x{th}）大于搜索图（{W}x{H}），无法匹配")
    tpl_full = _Tpl(g_tpl)
    if tpl_full.norm < 1e-6:
        raise RuntimeError("模板图像为纯色（无结构），无法匹配——请换一个含内容的模板区域")
    scale = max(1, min(tw, th) // 12)
    c_tpl = _Tpl(downsample(g_tpl, scale))
    phase_step = max(1, scale >> 1)
    cands = []
    tpl_zero = c_tpl.data - c_tpl.data.mean()
    tpl_norm = math.sqrt((tpl_zero * tpl_zero).sum())
    for oy in range(0, max(1, scale), phase_step):
        for ox in range(0, max(1, scale), phase_step):
            c_search = downsample(g_search, scale, ox, oy)
            ch, cw = c_search.shape
            if cw < c_tpl.w or ch < c_tpl.h:
                continue
            n = c_tpl.h * c_tpl.w
            sums, sqs = _window_sums(_Integrals(c_search), c_tpl.h, c_tpl.w)
            var = np.maximum(sqs - sums * sums / n, 0.0)
            oh, ow = ch - c_tpl.h + 1, cw - c_tpl.w + 1
            # 分子 Σ s·t̂（零均值模板下窗口均值项自动消去）：按模板行做一维相关再垂直累加，
            # 复杂度 O(HW·(th+tw))——避免展开 (oh,ow,th,tw) 巨型中间数组（4K 图达 GB 级）
            dots = np.zeros((oh, ow), dtype=np.float32)
            for ty in range(c_tpl.h):
                band = c_search[ty : ty + oh]
                win = sliding_window_view(band, c_tpl.w, axis=1)  # (oh, ow, tw)
                dots += np.einsum("xij,j->xi", win, tpl_zero[ty], optimize=True)
            # 平坦窗口（方差≈0）的 NCC 无定义：分母取下限 1e-6（与 _ncc_at 的 denom<1e-6 保护同构），
            # 否则残差会被放大成伪高分而挤占精化候选名额
            den = np.sqrt(var) * tpl_norm
            scores = np.where(den > 1e-6, dots / np.maximum(den, 1e-6), 0.0)
            ys_, xs_ = np.nonzero(scores > 0.4)
            for yy, xx in zip(ys_.tolist(), xs_.tolist()):
                cands.append((float(scores[yy, xx]), ox + xx * scale, oy + yy * scale))
    cands.sort(key=lambda c: -c[0])
    full_integ = _Integrals(g_search)
    radius = scale + 2
    refined = []
    for sc, cx, cy in cands[:16]:
        x0, y0 = max(0, cx - radius), max(0, cy - radius)
        x1, y1 = min(W - tw, cx + radius), min(H - th, cy + radius)
        best = None
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                s = _ncc_at(g_search, full_integ, tpl_full, x, y)
                if best is None or s > best[0]:
                    best = (s, x, y)
        if best:
            refined.append({"score": best[0], "x": best[1], "y": best[2], "w": tw, "h": th})
    refined.sort(key=lambda m: -m["score"])
    kept = []
    for m in refined:
        if m["score"] < threshold:
            break
        if any(abs(k["x"] - m["x"]) < m["w"] / 2 and abs(k["y"] - m["y"]) < m["h"] / 2 for k in kept):
            continue
        kept.append(m)
        if len(kept) >= 5:
            break
    return kept


# ---------------- 检测前后处理（对齐 TS core/cv/detect.ts） ----------------

PAD_GRAY = 114
NMS_IOU = 0.45
DETECT_CONF = 0.25


def letterbox(arr, size):
    """等比缩放至 size×size 居中放置（填充 114 灰）；返回 (NCHW, scale, padX, padY)。"""
    from PIL import Image
    import numpy as np

    H, W = arr.shape[:2]
    scale = min(size / W, size / H)
    w, h = max(1, round(W * scale)), max(1, round(H * scale))
    pad_x, pad_y = (size - w) // 2, (size - h) // 2
    resized = np.asarray(Image.fromarray(arr).resize((w, h), Image.BILINEAR), dtype=np.float32)
    canvas = np.full((size, size, 3), PAD_GRAY, dtype=np.float32)
    canvas[pad_y : pad_y + h, pad_x : pad_x + w] = resized
    chw = canvas / 255.0
    return chw.transpose(2, 0, 1)[None], scale, pad_x, pad_y


def iou(a, b):
    x1, y1 = max(a["x"], b["x"]), max(a["y"], b["y"])
    x2 = min(a["x"] + a["w"], b["x"] + b["w"])
    y2 = min(a["y"] + a["h"], b["y"] + b["h"])
    inter = max(0.0, x2 - x1) * max(0.0, y2 - y1)
    union = a["w"] * a["h"] + b["w"] * b["h"] - inter
    return inter / union if union > 0 else 0.0


def yolo_postprocess(output, dims, labels, src_w, src_h, scale, pad_x, pad_y, conf, iou_thr=NMS_IOU):
    """v8 [1,4+nc,N] / v5 [1,N,5+nc] 双形态后处理 + 按类 NMS（与 TS yoloPostprocess 同算法）。"""
    import numpy as np

    a, b = (dims[1] or 0), (dims[2] or 0)
    if a < 1 or b < 1:
        raise RuntimeError(f"YOLO 输出形状非法: {dims}")
    is_v8 = a < b
    nc = a - 4 if is_v8 else b - 5
    if nc < 1:
        raise RuntimeError(f"YOLO 类别数异常: {nc}")
    n = b if is_v8 else a
    candidates = []
    if is_v8:
        out = output.reshape(a, b)
        cx = out[0]
        cy = out[1]
        w = out[2]
        h = out[3]
        cls = out[4:]
        best_cls = cls.argmax(axis=0)
        best_score = cls.max(axis=0)
        for i in range(n):
            s = float(best_score[i])
            if s < conf:
                continue
            _push_cand(candidates, float(cx[i]), float(cy[i]), float(w[i]), float(h[i]), int(best_cls[i]), s, src_w, src_h, scale, pad_x, pad_y, labels)
    else:
        out = output.reshape(n, b)
        cx = out[:, 0]
        cy = out[:, 1]
        w = out[:, 2]
        h = out[:, 3]
        obj = out[:, 4]
        cls = out[:, 5:]
        scores = cls * obj[:, None]
        best_cls = scores.argmax(axis=1)
        best_score = scores.max(axis=1)
        for i in range(n):
            s = float(best_score[i])
            if s < conf:
                continue
            _push_cand(candidates, float(cx[i]), float(cy[i]), float(w[i]), float(h[i]), int(best_cls[i]), s, src_w, src_h, scale, pad_x, pad_y, labels)
    candidates.sort(key=lambda c: -c["score"])
    kept = []
    for c in candidates:
        if any(k["label"] == c["label"] and iou(k, c) > iou_thr for k in kept):
            continue
        kept.append(c)
    return kept


def _push_cand(cands, cx, cy, w, h, cls_idx, score, src_w, src_h, scale, pad_x, pad_y, labels):
    x1 = min(max((cx - w / 2 - pad_x) / scale, 0), src_w)
    y1 = min(max((cy - h / 2 - pad_y) / scale, 0), src_h)
    x2 = min(max((cx + w / 2 - pad_x) / scale, 0), src_w)
    y2 = min(max((cy + h / 2 - pad_y) / scale, 0), src_h)
    if x2 - x1 < 1 or y2 - y1 < 1:
        return
    cands.append({
        "x": x1, "y": y1, "w": x2 - x1, "h": y2 - y1,
        "label": labels[cls_idx] if cls_idx < len(labels) else f"class_{cls_idx}",
        "score": score,
    })


def pair_objects_with_text(objects, lines):
    """检测框×OCR 行几何配对：行中心落在框内即归属，行内按阅读序空格拼接（与 TS 同）。"""
    sorted_lines = sorted(lines, key=lambda l: (l["y"], l["x"]))
    out = []
    for o in objects:
        texts = [
            l["text"].strip()
            for l in sorted_lines
            if o["x"] <= l["x"] + l["w"] / 2 <= o["x"] + o["w"]
            and o["y"] <= l["y"] + l["h"] / 2 <= o["y"] + o["h"]
            and l["text"].strip()
        ]
        item = dict(o)
        if texts:
            item["text"] = " ".join(texts)
        out.append(item)
    return out


# ---------------- ONNX 元数据（ultralytics imgsz/names 自适应） ----------------


def _read_varint(b, p):
    val = 0
    shift = 0
    while p < len(b):
        byte = b[p]
        p += 1
        val |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return val, p
        shift += 7
        if shift > 63:
            return None, p
    return None, p


def parse_onnx_metadata(path):
    """解析 ONNX protobuf 的 StringStringProto 条目（producer 相邻 key/value）——
    ultralytics 导出内嵌 imgsz/names（与 TS onnx-meta.ts 同实现约定）。"""
    with open(path, "rb") as f:
        data = f.read()
    props = {}
    i = 0
    n = len(data)
    last_key = None
    while i < n:
        field = data[i]
        i += 1
        if field & 0x80:  # 非法/不支持的两字节 field id：终止（够用即可）
            break
        wire = field & 0x7
        if wire == 0:
            v, i = _read_varint(data, i)
            if v is None:
                break
        elif wire == 2:
            ln, i = _read_varint(data, i)
            if ln is None or i + ln > n:
                break
            payload = data[i : i + ln]
            i += ln
            try:
                s = payload.decode("utf-8")
            except UnicodeDecodeError:
                s = None
            if s is not None and all(32 <= ord(c) < 127 or c in "\n\r\t" for c in s):
                if last_key is not None:
                    props.setdefault(last_key, s)
                    last_key = None
                else:
                    last_key = s
        elif wire == 5:
            i += 4
        elif wire == 1:
            i += 8
        else:
            break
    return props


def ultralytics_meta(props):
    """从元数据提取 imgsz 与 names 列表（py dict 字符串解析，与 TS parsePyDict 约定一致）。"""
    names = None
    imgsz = None
    raw_names = props.get("names")
    if raw_names:
        s = raw_names.strip()
        if s.startswith("{") and s.endswith("}"):
            body = s[1:-1]
            items = []
            for part in body.split(","):
                if not part.strip():
                    continue
                k, _, v = part.partition(":")
                v = v.strip().strip("'\"")
                if v:
                    items.append(v)
            names = items
        else:
            names = [v.strip().strip("'\"") for v in raw_names.split(",") if v.strip()]
    raw_sz = props.get("imgsz")
    if raw_sz:
        digits = "".join(c if c.isdigit() else " " for c in raw_sz)
        vals = [int(v) for v in digits.split()]
        if vals:
            imgsz = max(vals)
    return {"names": names, "imgsz": imgsz}


# ---------------- OCR 推理 ----------------

_OCR = {"chars": None}


def _ocr_assets():
    d = resolve_ocr_dir()
    if not d:
        raise RuntimeError(
            "OCR 模型未配置：请设置 GEBAI_CV_MODELS_DIR 指向含 det.onnx / rec.onnx / dict.txt 的目录"
            "（PP-OCR 中英文三件套；源码形态可运行 scripts/build-cv-embed.ts 下载到 packages/server/assets/cv-models/）"
        )
    det = os.path.join(d, "det.onnx")
    rec = os.path.join(d, "rec.onnx")
    dictf = os.path.join(d, "dict.txt")
    if not (os.path.isfile(det) and os.path.isfile(rec) and os.path.isfile(dictf)):
        raise RuntimeError(f"OCR 模型目录不完整（需 det.onnx / rec.onnx / dict.txt）: {d}")
    if _OCR["chars"] is None:
        with open(dictf, "r", encoding="utf-8", errors="replace") as f:
            chars = [line.rstrip("\r\n") for line in f if line.strip()]
        # 与 TS cv.ts 同构：index 0 为占位符（CTC blank=0 不出字），字典尾追加空格
        _OCR["chars"] = ["\ufffd", *chars, " "]
    return det, rec, _OCR["chars"]


def _detect_boxes(img_arr, max_side=DET_MAX_SIDE):
    """det → DB 文本框（img_arr 像素系）。"""
    import numpy as np

    det_p, _rec_p, _chars = _ocr_assets()
    H, W = img_arr.shape[:2]
    inp, _pw, _ph, scale = det_preprocess(img_arr, max_side)
    sess = _session(det_p)
    out = sess.run(None, {sess.get_inputs()[0].name: inp})[0]
    prob = np.asarray(out[0, 0], dtype=np.float32)
    return db_postprocess(prob, prob.shape[1], prob.shape[0], W, H, scale)


def _recognize(img_arr, boxes):
    """DB 框 → 逐行识别文本（坐标与 img_arr 同系）。"""
    import numpy as np

    _det_p, rec_p, chars = _ocr_assets()
    rec_sess = _session(rec_p)
    lines = []
    for b in boxes:
        x, y = int(round(b["x"])), int(round(b["y"]))
        w, h = max(1, int(round(b["w"]))), max(1, int(round(b["h"])))
        crop = img_arr[y : y + h, x : x + w]
        if crop.shape[0] < 2 or crop.shape[1] < 2:
            continue
        rin = rec_preprocess(crop)
        rout = rec_sess.run(None, {rec_sess.get_inputs()[0].name: rin})[0]
        probs = np.asarray(rout[0], dtype=np.float32)  # (steps, classes)
        text, score = ctc_decode(probs, chars)
        if text:
            lines.append({"text": text, "score": round(float(score), 3), "x": b["x"], "y": b["y"], "w": b["w"], "h": b["h"]})
    return lines


def _iou_box(a, b):
    """两框交并比（分块重叠区去重用）。"""
    iw = min(a["x"] + a["w"], b["x"] + b["w"]) - max(a["x"], b["x"])
    ih = min(a["y"] + a["h"], b["y"] + b["h"]) - max(a["y"], b["y"])
    if iw <= 0 or ih <= 0:
        return 0.0
    inter = iw * ih
    union = a["w"] * a["h"] + b["w"] * b["h"] - inter
    return inter / union if union > 0 else 0.0


def _merge_tile_lines(lines):
    """分块结果去重：重叠区同一条行被相邻块各识别一次时保留置信度更高者，再按阅读序排序。"""
    kept = []
    for l in sorted(lines, key=lambda x: -x["score"]):
        if any(_iou_box(l, k) > OCR_MERGE_IOU for k in kept):
            continue
        kept.append(l)
    kept.sort(key=lambda l: (l["y"], l["x"]))
    return kept


def ocr_lines(img_arr, max_side=DET_MAX_SIDE):
    """完整 OCR：det → DB 框 → rec。返回 [{text, score, x, y, w, h}]（原图像素系）。

    大图自适应分块：长边超过 OCR_TILE_SIDE 时按块检测（每块以原生分辨率输入模型，
    重叠区按框 IoU 去重）——整图等比缩到长边 max_side 会把小字号压到不可辨识，
    分块后小字保持原生像素尺度。
    """

    _need_deps()
    H, W = img_arr.shape[:2]
    if max(H, W) <= OCR_TILE_SIDE:
        return _recognize(img_arr, _detect_boxes(img_arr, max_side))
    tile_side = max(OCR_TILE_SIDE, max_side)
    step = max(1, OCR_TILE_SIDE - OCR_TILE_OVERLAP)
    lines = []
    y = 0
    while y < H:
        x = 0
        while x < W:
            tile = img_arr[y : min(y + OCR_TILE_SIDE, H), x : min(x + OCR_TILE_SIDE, W)]
            if tile.shape[0] >= 8 and tile.shape[1] >= 8:
                for l in _recognize(tile, _detect_boxes(tile, tile_side)):
                    lines.append({**l, "x": l["x"] + x, "y": l["y"] + y})
            if x + OCR_TILE_SIDE >= W:
                break
            x += step
        if y + OCR_TILE_SIDE >= H:
            break
        y += step
    return _merge_tile_lines(lines)


OCR_LINE_LIMIT = 200


def tool_ocr(args):
    """ocr：本地 OCR 识别图片文字（PP-OCR 中英文小模型，onnxruntime 原生推理——带图片像素坐标、离线不耗配额）。"""
    import numpy as np

    if not str(args.get("image") or ""):
        return {"output": "缺少 image 参数：PNG 图片路径（必填——本工具无缺省图像源，需要现截屏幕/页面用 desktop/playwright 子代理）"}
    region = str(args.get("region") or "")
    off_x = off_y = 0
    try:
        img = np.asarray(load_image(args["image"]), dtype=np.uint8)
        if region:
            r = parse_region(region)
            if not r:
                return {"output": f"region 格式错误: {region}（应为 x,y,w,h）"}
            img, off_x, off_y = crop_arr(img, r)
        lines = ocr_lines(img)
    except RuntimeError as e:
        return {"output": str(e)}
    find = str(args.get("find") or "").strip()
    if find:
        lines = [l for l in lines if find in l["text"]]
    lines = [
        {**l, "x": l["x"] + off_x, "y": l["y"] + off_y}
        for l in lines
    ]
    truncated = len(lines) > OCR_LINE_LIMIT
    lines = lines[:OCR_LINE_LIMIT]
    if not lines:
        tail = f"（未命中「{find}」；用不带 find 的调用读取全部文字确认实际措辞）" if find else "（未识别到文字——图片可能无文本/分辨率过低；可改用 locate_image 模板匹配或 vision_analyze 语义分析）"
        return {"output": tail.strip()}
    out_lines = [f"{l['text']}  [{round(l['x'])},{round(l['y'])} {round(l['w'])}x{round(l['h'])}]  {l['score']}" for l in lines]
    head = f"找到 {len(lines)} 行文字（图片像素系，坐标已映射回原图）：" if not find else f"命中「{find}」{len(lines)} 行（图片像素系）："
    if truncated:
        head += f"\n（结果截断至 {OCR_LINE_LIMIT} 行——用 find 关键词过滤或 region 限定区域收窄）"
    return {"output": head + "\n" + "\n".join(out_lines), "data": {"lines": lines}}


def tool_locate(args):
    """locate：在图片中定位目标文字的精确像素坐标（本地 OCR）。"""
    import numpy as np

    target = str(args.get("target") or "").strip()
    if not target:
        return {"output": "缺少 target 参数：要定位的文字"}
    if not str(args.get("image") or ""):
        return {"output": "缺少 image 参数：PNG 图片路径（必填）"}
    region = str(args.get("region") or "")
    off_x = off_y = 0
    try:
        img = np.asarray(load_image(args["image"]), dtype=np.uint8)
        if region:
            r = parse_region(region)
            if not r:
                return {"output": f"region 格式错误: {region}（应为 x,y,w,h）"}
            img, off_x, off_y = crop_arr(img, r)
        lines = ocr_lines(img)
    except RuntimeError as e:
        return {"output": str(e)}
    cands = [
        {**l, "x": l["x"] + off_x, "y": l["y"] + off_y, "cx": round(l["x"] + off_x + l["w"] / 2), "cy": round(l["y"] + off_y + l["h"] / 2)}
        for l in lines
        if target in l["text"]
    ]
    if not cands:
        return {
            "output": (
                f"未找到「{target}」。建议：1) 用 vision_ocr 读取图片全部文字确认实际措辞；"
                "2) 文字可能是图标/图形，改用 vision_locate_image（模板匹配）；3) 需语义理解改用 vision_analyze。"
            )
        }
    best = max(cands, key=lambda c: c["score"])
    cands.sort(key=lambda c: -c["score"])
    out = [
        f"最佳匹配: 「{best['text']}」 中心 ({best['cx']}, {best['cy']})  置信度 {best['score']}  [框 {round(best['x'])},{round(best['y'])} {round(best['w'])}x{round(best['h'])}]",
        f"坐标为图片像素系——消费方自行映射到目标环境（屏幕经 desktop 子代理加窗口原点、页面经 playwright elementFromPoint）",
    ]
    if len(cands) > 1:
        out.append(f"其余候选（{len(cands) - 1} 个，按置信度降序）: " + "; ".join(f"「{c['text']}」({c['cx']},{c['cy']}) {c['score']}" for c in cands[1:6]))
    return {"output": "\n".join(out), "data": {"best": best, "candidates": cands}}


def tool_locate_image(args):
    """locate_image：模板匹配定位图标/图形元素（零训练，同尺寸匹配）。"""
    import numpy as np

    image = str(args.get("image") or "")
    if not image:
        return {"output": "缺少 image 参数：搜索 PNG 图片路径（必填）"}
    template = str(args.get("template") or "")
    tpl_region = str(args.get("template_region") or "")
    if not template and not tpl_region:
        return {"output": "template 与 template_region 二选一（模板 PNG 路径 / 在搜索图坐标内取模板区域）"}
    region = str(args.get("region") or "")
    try:
        img = np.asarray(load_image(image), dtype=np.uint8)
        if region:
            r = parse_region(region)
            if not r:
                return {"output": f"region 格式错误: {region}（应为 x,y,w,h）"}
            img, off_x, off_y = crop_arr(img, r)
        else:
            off_x = off_y = 0
        if template:
            tpl = np.asarray(load_image(template), dtype=np.uint8)
        else:
            r = parse_region(tpl_region)
            if not r:
                return {"output": f"template_region 格式错误: {tpl_region}（应为 x,y,w,h，搜索图坐标系）"}
            tpl, _, _ = crop_arr(img, r)
        matches = match_template(img, tpl)
    except RuntimeError as e:
        return {"output": str(e)}
    if not matches:
        return {
            "output": (
                "未匹配到（阈值 0.8）。建议：1) 确认模板与目标同尺寸（本工具不做缩放匹配，模板宜从同一环境截图裁剪）；"
                "2) 目标可能是文字——改用 vision_locate；3) 用 vision_analyze 对图片做语义分析。"
            )
        }
    out = []
    for m in matches:
        cx, cy = round(m["x"] + off_x + m["w"] / 2), round(m["y"] + off_y + m["h"] / 2)
        out.append(f"匹配 {round(m['score'], 3)}: 中心 ({cx}, {cy})  [框 {round(m['x'] + off_x)},{round(m['y'] + off_y)} {m['w']}x{m['h']}]（图片像素系）")
    return {"output": f"找到 {len(matches)} 处匹配（按相似度降序，图片像素系）：\n" + "\n".join(out), "data": {"matches": [{**m, "x": m["x"] + off_x, "y": m["y"] + off_y} for m in matches]}}


def tool_detect(args):
    """detect：本地 YOLO 目标检测（自备 ultralytics ONNX，元数据自适应；OCR 配对输出每框文本）。"""
    import numpy as np

    _need_deps()
    model, err = resolve_detect_model()
    if err:
        return {"output": err}
    image = str(args.get("image") or "")
    if not image:
        return {"output": "缺少 image 参数：PNG 图片路径（必填——本工具无缺省图像源）"}
    conf = float(args.get("conf") or 0.25)
    iou_thr = float(args.get("iou") or 0.45)
    pair_text = args.get("pair_text")
    pair_text = True if pair_text is None else bool(pair_text)
    region = str(args.get("region") or "")
    off_x = off_y = 0
    try:
        img = np.asarray(load_image(image), dtype=np.uint8)
        if region:
            r = parse_region(region)
            if not r:
                return {"output": f"region 格式错误: {region}（应为 x,y,w,h）"}
            img, off_x, off_y = crop_arr(img, r)
        # 标签：ultralytics 元数据自适应 → GEBAI_CV_DETECT_LABELS → 约定 class_i
        import driver

        props = parse_onnx_metadata(model)
        meta = ultralytics_meta(props)
        labels = meta["names"]
        labels_env = driver.ctx_env("GEBAI_CV_DETECT_LABELS")
        if labels_env and os.path.isfile(labels_env):
            with open(labels_env, "r", encoding="utf-8", errors="replace") as f:
                labels = [line.strip() for line in f if line.strip()]
        sess = _session(model)
        size = meta["imgsz"]
        if not size:
            # 元数据缺 imgsz：读会话输入形状的静态空间维兜底（如 [1,3,1280,1280] → 1280）
            try:
                shp = [d for d in sess.get_inputs()[0].shape if isinstance(d, int)]
                size = max(shp[1:]) if len(shp) >= 3 else 0
            except Exception:
                size = 0
        size = size or 640
        inp, scale, pad_x, pad_y = letterbox(img, size)
        out = sess.run(None, {sess.get_inputs()[0].name: inp})[0]
        dims = [1] + [d for d in np.asarray(out).shape[1:] if d] or [1, 1, 1]
        arr = np.asarray(out, dtype=np.float32).reshape(-1)
        nc_est = dims[1] - 4 if dims[1] < (dims[2] or 0) else (dims[2] or 0) - 5
        if not labels or len(labels) < nc_est:
            labels = (labels or []) + [f"class_{i}" for i in range(max(nc_est, 1))]
        H, W = img.shape[:2]
        objs = yolo_postprocess(arr, dims, labels, W, H, scale, pad_x, pad_y, conf, iou_thr)
        objs = [{**o, "x": o["x"] + off_x, "y": o["y"] + off_y} for o in objs]
        if pair_text and objs:
            try:
                lines = ocr_lines(img)
                lines = [{**l, "x": l["x"] + off_x, "y": l["y"] + off_y} for l in lines]
                objs = pair_objects_with_text(objs, lines)
            except RuntimeError:
                pass  # OCR 未配置：跳过配对，仅输出检测框
    except RuntimeError as e:
        return {"output": str(e)}
    if not objs:
        return {"output": "未检测到目标。可尝试降低 conf 阈值，或确认模型/标签与场景匹配；需语义理解改用 vision_analyze。"}
    out = [
        f"{o['label']}  {round(o['score'], 3)}  [{round(o['x'])},{round(o['y'])} {round(o['w'])}x{round(o['h'])}]（图片像素系）"
        + (f"  文本: {o['text']}" if o.get("text") else "")
        for o in objs
    ]
    return {"output": f"检测到 {len(objs)} 个目标（onnxruntime 原生推理）：\n" + "\n".join(out), "data": {"objects": objs}}


TOOLS = [
    {
        "name": "ocr",
        "description": "本地 OCR 识别图片文字（PP-OCR 中英文小模型，onnxruntime 原生推理——离线运行、快、带图片像素坐标、不耗模型配额）。image 为 PNG 图片路径（必填，相对路径基准=会话工作区）；region 限定区域（'x,y,w,h'，图片内坐标）；find 关键词过滤。返回每行文字与图片像素坐标、置信度。",
        "parameters": {
            "type": "object",
            "properties": {
                "image": {"type": "string", "description": "PNG 图片路径（相对会话工作目录，必填——本工具无缺省图像源）"},
                "region": {"type": "string", "description": "可选：识别区域 'x,y,w,h'（图片内像素坐标）"},
                "find": {"type": "string", "description": "可选：关键词过滤（只返回含该词的行）"},
            },
            "required": ["image"],
        },
    },
    {
        "name": "locate",
        "description": "在图片中定位目标文字的精确像素坐标（本地 OCR）。target 为要找的文字；image 为 PNG 图片路径（必填）；region 限定搜索区域。返回最佳匹配的中心坐标（图片像素系）与全部候选。",
        "parameters": {
            "type": "object",
            "properties": {
                "target": {"type": "string", "description": "要定位的文字"},
                "image": {"type": "string", "description": "PNG 图片路径（相对会话工作目录，必填）"},
                "region": {"type": "string", "description": "可选：搜索区域 'x,y,w,h'（图片内像素坐标）"},
            },
            "required": ["target", "image"],
        },
    },
    {
        "name": "locate_image",
        "description": "在图片中按模板定位图标/图形元素（本地模板匹配，零训练）。template 为模板 PNG 路径，或 template_region 从搜索图坐标内取模板区域；同尺寸匹配（模板需与目标显示尺寸一致）。image 为搜索 PNG 图片路径（必填）；region 限定搜索区域。",
        "parameters": {
            "type": "object",
            "properties": {
                "image": {"type": "string", "description": "PNG 搜索图路径（相对会话工作目录，必填）"},
                "template": {"type": "string", "description": "模板 PNG 路径（与 template_region 二选一）"},
                "template_region": {"type": "string", "description": "可选：'x,y,w,h' 在搜索图坐标系内取模板区域（与 template 二选一；坐标系同 vision_ocr 对同一 image/region 的输出）"},
                "region": {"type": "string", "description": "可选：搜索区域 'x,y,w,h'（图片内像素坐标）"},
            },
            "required": ["image"],
        },
    },
    {
        "name": "detect",
        "description": "本地目标检测（自备 YOLO ONNX 模型：GEBAI_CV_DETECT_MODEL 指定，或放入 {GEBAI_HOME}/models/detect/ 唯一 .onnx 自动生效；ultralytics 导出的 ONNX 自动读取内嵌输入尺寸与类别，免标签配置）。返回检测对象类别与图片像素坐标，并默认与 OCR 配对输出每框文本（pair_text 可关）。image 为 PNG 图片路径（必填）；conf 置信度阈值（默认 0.25）；iou NMS 阈值（默认 0.45，密集小控件场景建议 0.1）。",
        "parameters": {
            "type": "object",
            "properties": {
                "image": {"type": "string", "description": "PNG 图片路径（相对会话工作目录，必填——本工具无缺省图像源）"},
                "conf": {"type": "number", "description": "置信度阈值（默认 0.25）"},
                "iou": {"type": "number", "description": "NMS IoU 阈值（默认 0.45）"},
                "pair_text": {"type": "boolean", "description": "是否与 OCR 配对输出每框文本（默认 true）"},
                "region": {"type": "string", "description": "可选：检测区域 'x,y,w,h'（图片内像素坐标）"},
            },
            "required": ["image"],
        },
    },
]

TOOL_IMPLS = {"ocr": tool_ocr, "locate": tool_locate, "locate_image": tool_locate_image, "detect": tool_detect}
