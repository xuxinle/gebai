#!/usr/bin/env python3
"""vision 边车工具的纯函数测试。

选这些函数是因为它们**不依赖 numpy/onnxruntime**（第三方依赖在调用时才 import），
但承载了容易出错的几何/路径逻辑：区域解析、分块 OCR 去重、资源目录候选顺序。
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from tests.run_tests import Skip, load_module  # noqa: E402

VISION_TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "vision", "tools.py")
tools = load_module(VISION_TOOLS, "keqing_vision_tools")


def test_parse_region_ascii_and_fullwidth_comma():
    assert tools.parse_region("10,20,30,40") == [10.0, 20.0, 30.0, 40.0]
    # 中文逗号：用户/模型都可能这么写，解析不能因此失败
    assert tools.parse_region("10，20，30，40") == [10.0, 20.0, 30.0, 40.0]


def test_parse_region_rejects_bad_input():
    assert tools.parse_region("10,20,30") is None  # 数量不对
    assert tools.parse_region("a,b,c,d") is None  # 非数字
    assert tools.parse_region("") is None
    assert tools.parse_region(None) is None


def test_iou_box_identity_and_disjoint():
    box = {"x": 0, "y": 0, "w": 10, "h": 10}
    assert tools._iou_box(box, dict(box)) == 1.0
    far = {"x": 100, "y": 100, "w": 10, "h": 10}
    assert tools._iou_box(box, far) == 0.0


def test_iou_box_partial_overlap_matches_manual_value():
    a = {"x": 0, "y": 0, "w": 10, "h": 10}
    b = {"x": 5, "y": 5, "w": 10, "h": 10}
    # 交集 5*5=25；并集 100+100-25=175
    assert abs(tools._iou_box(a, b) - (25 / 175)) < 1e-9


def _line(text, score, x, y, w=40, h=10):
    return {"text": text, "score": score, "x": x, "y": y, "w": w, "h": h}


def test_merge_tile_lines_dedupes_overlap_keeping_higher_score():
    # 重叠区同一条行被相邻块各识别一次：保留置信度更高者
    low = _line("你好", 0.60, 100, 50)
    high = _line("你好", 0.95, 102, 51)
    merged = tools._merge_tile_lines([low, high])
    assert len(merged) == 1
    assert merged[0]["score"] == 0.95


def test_merge_tile_lines_sorts_reading_order():
    second = _line("第二行", 0.9, 10, 80)
    first = _line("第一行", 0.9, 10, 20)
    merged = tools._merge_tile_lines([second, first])
    assert [l["text"] for l in merged] == ["第一行", "第二行"]


def test_resource_candidates_prefer_new_layout(tmp_path="/tmp"):
    # 新资源目录优先、旧 models/ 回退（与 TS core/cv/resources.ts 同口径）
    previous = os.environ.get("GEBAI_HOME")
    os.environ["GEBAI_HOME"] = tmp_path
    try:
        candidates = tools._resource_candidates(("resources", "models", "cv", "ocr"), ("models", "ocr"))
    finally:
        if previous is None:
            os.environ.pop("GEBAI_HOME", None)
        else:
            os.environ["GEBAI_HOME"] = previous
    assert len(candidates) == 2
    assert candidates[0] == os.path.join(tmp_path, "resources", "models", "cv", "ocr")
    assert candidates[1] == os.path.join(tmp_path, "models", "ocr")


def test_resolve_ocr_dir_requires_model_files_and_prefers_new_layout():
    """目录只有"三件套齐备"才算可用：目录存在但没有 det.onnx 时不能返回（否则把错误推迟到运行期）。"""
    import tempfile

    home = tempfile.mkdtemp(prefix="keqing-vision-")
    new_dir = os.path.join(home, "resources", "models", "cv", "ocr")
    os.makedirs(new_dir, exist_ok=True)

    previous_home = os.environ.get("GEBAI_HOME")
    previous_models = os.environ.get("GEBAI_CV_MODELS_DIR")
    os.environ["GEBAI_HOME"] = home
    os.environ.pop("GEBAI_CV_MODELS_DIR", None)
    try:
        # ① 目录在但模型缺失 → 不返回（不能给出一个后续加载才报错的路径）
        assert tools.resolve_ocr_dir() is None
        # ② 放入 det.onnx 后按新布局命中
        with open(os.path.join(new_dir, "det.onnx"), "wb") as handle:
            handle.write(b"stub")
        assert tools.resolve_ocr_dir() == new_dir
        # ③ 显式环境变量是回退链第一顺位
        override = os.path.join(home, "explicit")
        os.makedirs(override, exist_ok=True)
        with open(os.path.join(override, "det.onnx"), "wb") as handle:
            handle.write(b"stub")
        os.environ["GEBAI_CV_MODELS_DIR"] = override
        assert tools.resolve_ocr_dir() == override
    finally:
        for key, value in (("GEBAI_HOME", previous_home), ("GEBAI_CV_MODELS_DIR", previous_models)):
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def test_crop_arr_clamps_out_of_range_region():
    np = None
    try:
        import numpy  # noqa: PLC0415
    except ImportError:
        skip("未安装 numpy（vision_pip install numpy 后可测）")
    np = numpy
    arr = np.arange(4 * 5 * 3, dtype="uint8").reshape(4, 5, 3)  # H=4 W=5
    # 越界起点会被钳制到图内，宽高至少 1（不能产生空裁剪）
    cropped, off_x, off_y = tools.crop_arr(arr, [10, 10, 4, 4])
    assert cropped.shape[0] >= 1 and cropped.shape[1] >= 1
    assert off_x <= 4 and off_y <= 3
    # 正常区域内裁剪尺寸正确
    cropped2, x2, y2 = tools.crop_arr(arr, [1, 1, 2, 3])
    assert cropped2.shape == (3, 2, 3)
    assert (x2, y2) == (1, 1)
