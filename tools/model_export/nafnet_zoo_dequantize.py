#!/usr/bin/env python3
"""Fold the OpenCV Zoo NAFNet graph's constant blocked weights to FP32.

The output is for local evaluation. The separate reference tool must compare
its output with the original graph before it can become a model pack.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np
import onnx
from onnx import helper, numpy_helper

SOURCE_SHA256 = "07263f416febeccE10193dd648e950b22e397cf521eedab1a114ef77b2bc9587".lower()


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, help="Downloaded Zoo ONNX graph")
    parser.add_argument("license", type=Path, help="Zoo LICENSE file")
    parser.add_argument("destination", type=Path, help="New local evaluation directory")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    license_file = args.license.resolve(strict=True)
    if sha256(source) != SOURCE_SHA256:
        raise SystemExit("The source graph SHA-256 does not match the evaluated Zoo ONNX")
    if args.destination.exists():
        raise SystemExit("Refusing to overwrite an existing destination")

    model = onnx.load(str(source))
    initializers = {item.name: item for item in model.graph.initializer}
    folded = []
    kept_nodes = []
    quant_inputs = set()
    for node in model.graph.node:
        if node.op_type != "DequantizeLinear":
            kept_nodes.append(node)
            continue
        if len(node.input) != 3 or len(node.output) != 1 or any(name not in initializers for name in node.input):
            raise ValueError(f"Nonconstant dequantization is not supported: {node.name}")
        values, scales, zeros = [numpy_helper.to_array(initializers[name]) for name in node.input]
        attributes = {item.name: helper.get_attribute_value(item) for item in node.attribute}
        axis = int(attributes.get("axis", 1)) % values.ndim
        block = int(attributes.get("block_size", 0))
        if (values.dtype != np.int8 or scales.dtype != np.float32 or zeros.dtype != np.int8 or
                values.ndim != scales.ndim or scales.shape != zeros.shape or block <= 0 or
                any(values.shape[i] != scales.shape[i] for i in range(values.ndim) if i != axis) or
                (values.shape[axis] + block - 1) // block != scales.shape[axis]):
            raise ValueError(f"Unexpected blocked quantization contract: {node.name}")
        selection = [slice(None)] * values.ndim
        selection[axis] = slice(0, values.shape[axis])
        expanded_scales = np.repeat(scales, block, axis=axis)[tuple(selection)]
        expanded_zeros = np.repeat(zeros, block, axis=axis)[tuple(selection)]
        restored = (values.astype(np.float32) - expanded_zeros.astype(np.float32)) * expanded_scales
        folded.append(numpy_helper.from_array(restored.astype(np.float32, copy=False),node.output[0]))
        quant_inputs.update(node.input)
    if len(folded) != 447:
        raise ValueError(f"Expected 447 constant dequantization nodes, found {len(folded)}")

    remaining_names = {name for node in kept_nodes for name in node.input if name}
    remaining_names.update(value.name for value in model.graph.input)
    remaining_names.update(value.name for value in model.graph.output)
    retained = [item for item in model.graph.initializer if item.name not in quant_inputs or item.name in remaining_names]
    del model.graph.node[:]
    model.graph.node.extend(kept_nodes)
    del model.graph.initializer[:]
    model.graph.initializer.extend(retained)
    model.graph.initializer.extend(folded)
    onnx.checker.check_model(model)

    args.destination.mkdir(parents=True)
    graph = args.destination / "model.onnx"
    onnx.save(model, str(graph))
    shutil.copyfile(license_file, args.destination / "LICENSE")
    report = {"source_sha256": SOURCE_SHA256, "derived_sha256": sha256(graph),
              "folded_dequantize_nodes": len(folded), "method": "constant blocked INT8 weights to FP32",
              "status": "pending_cpu_reference", "distribution": "local evaluation only"}
    (args.destination / "conversion.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
