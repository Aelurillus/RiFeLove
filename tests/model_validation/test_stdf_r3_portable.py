"""Developer-only CPU export primitives; torch/torchvision are not app runtime deps."""
from pathlib import Path
import importlib.util

import pytest

pytest.importorskip("torch")
pytest.importorskip("torchvision")
pytest.importorskip("onnxruntime")


def load(relative):
    path = Path(__file__).resolve().parents[2] / relative
    spec = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


portable = load("tools/model_export/stdf_r3_portable.py")
quality = load("tools/validation/stdf_r3_cpu_quality.py")


def test_portable_deformation_matches_independent_cpu_operator():
    portable.torch.set_num_threads(2)
    cases = portable.operator_cases()
    assert len(cases) == 9
    assert all(case["passed"] for case in cases)


@pytest.mark.parametrize("center,expected", [
    (0, [0, 0, 0, 0, 1, 2, 3]),
    (3, [0, 1, 2, 3, 3, 3, 3]),
    (4, [4, 4, 4, 4, 5, 6, 7]),
    (8, [5, 6, 7, 8, 8, 8, 8]),
])
def test_temporal_window_never_crosses_known_cut(center, expected):
    assert quality.scene_window(center, 9, [4]) == expected
    assert quality.scene_window(center, 9, [4], repeat=True) == [center] * 7


def test_invalid_temporal_geometry_fails():
    with pytest.raises(ValueError):
        quality.scene_window(-1, 9, [4])
    with pytest.raises(ValueError):
        quality.scene_window(0, 9, [9])
    with pytest.raises(ValueError):
        portable.PortableDeformConv(7, 64, 3, stride=2, padding=1, deformable_groups=7)


def test_ssim_reference_identity_and_perturbation():
    reference = quality.fixture(48, 32, 9, 4)
    assert quality.ssim(reference, reference) == pytest.approx(1., abs=1e-12)
    assert quality.ssim(255 - reference, reference) < .5
