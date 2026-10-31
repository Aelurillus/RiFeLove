"""Local model-pack integrity checks using the reviewed evaluation artifacts."""
import importlib.util
import json
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
path = ROOT / "tools/model_export/stdf_local_pack.py"
spec = importlib.util.spec_from_file_location(path.stem, path)
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


@pytest.fixture
def evidence():
    graph = ROOT / "work/verification/stdf-r3-portable-cpu-20261031-v3/stdf-r3-y7-fp32.onnx"
    cpu = graph.with_name("feasibility.json")
    cuda = ROOT / "work/verification/stdf-cuda-reference-20261031-v2/cuda-reference.json"
    native = ROOT / "work/verification/stdf-cuda-shared-runtime-20261031-r14/report.json"
    if not all(p.is_file() for p in (graph, cpu, cuda, native)):
        pytest.skip("local STDF graph and evaluation evidence are not installed")
    return graph, cpu, cuda, native


def test_local_pack_preserves_graph_and_declares_unverified_weight_rights(evidence, tmp_path):
    output = tmp_path / "pack"
    pack.create_pack(*evidence, output)
    descriptor = json.loads((output / "model.json").read_text(encoding="utf-8"))
    assert pack.digest((output / descriptor["modelFile"]).read_bytes()) == descriptor["sha256"]
    for section, name_key, hash_key in (("license", "file", "sha256"),
                                        ("provenance", "manifestFile", "manifestSha256"),
                                        ("validation", "reportFile", "reportSha256")):
        entry = descriptor[section]
        assert pack.digest((output / entry[name_key]).read_bytes()) == entry[hash_key]
    license_text = (output / descriptor["license"]["file"]).read_text(encoding="utf-8")
    assert "rights" in license_text.lower() and "not been established" in license_text.lower()
    assert "local evaluation only" in license_text.lower()
    assert descriptor["capabilities"]["tasks"] == ["deblock"]


def test_rejected_evidence_and_existing_output_cannot_replace_a_pack(evidence, tmp_path):
    graph, cpu, cuda, native = evidence
    bad = json.loads(native.read_text(encoding="utf-8"))
    bad["cpuImageFallbackDetected"] = True
    bad_path = tmp_path / "bad.json"
    bad_path.write_text(json.dumps(bad), encoding="utf-8")
    output = tmp_path / "pack"
    with pytest.raises(ValueError, match="provider assignment"):
        pack.create_pack(graph, cpu, cuda, bad_path, output)
    assert not output.exists()
    missing_shape = json.loads(cuda.read_text(encoding="utf-8"))
    missing_shape["cases"] = [case for case in missing_shape["cases"] if case.get("name") != "cuda-384x256"]
    incomplete_path = tmp_path / "incomplete-cuda.json"
    incomplete_path.write_text(json.dumps(missing_shape), encoding="utf-8")
    with pytest.raises(ValueError, match="required shapes"):
        pack.create_pack(graph, cpu, incomplete_path, native, output)
    assert not output.exists()
    output.mkdir()
    sentinel = output / "user-file.txt"
    sentinel.write_text("keep", encoding="utf-8")
    with pytest.raises(FileExistsError):
        pack.create_pack(graph, cpu, cuda, native, output)
    assert sentinel.read_text(encoding="utf-8") == "keep"
