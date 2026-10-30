"""Prove TensorRT engine builds use the selected runtime when PATH has an older DLL first."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


CLI, FFMPEG, FFPROBE, MODEL_PACK, SHADOW_RUNTIME, SELECTED_RUNTIME = map(
    lambda value: Path(value).resolve(), sys.argv[1:7]
)


def invoke(command, env, timeout=240):
    result = subprocess.run([str(item) for item in command], capture_output=True,
                            text=True, encoding="utf-8", errors="replace", env=env,
                            timeout=timeout)
    if result.returncode:
        raise AssertionError(
            f"{command[0]} exited with {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main():
    for name in ("nvinfer_builder_resource_10.dll", "onnxruntime_providers_tensorrt.dll"):
        if not (SHADOW_RUNTIME / name).is_file() or not (SELECTED_RUNTIME / name).is_file():
            raise AssertionError(f"runtime-precedence fixture is missing {name}")
    if (SHADOW_RUNTIME / "nvinfer_builder_resource_10.dll").stat().st_size == \
            (SELECTED_RUNTIME / "nvinfer_builder_resource_10.dll").stat().st_size:
        raise AssertionError("shadow and selected TensorRT builder DLLs must differ for this regression")

    with tempfile.TemporaryDirectory(prefix="rifelove-tensorrt-path-", dir=CLI.parent) as temporary:
        root = Path(temporary)
        source = root / "tagged-320x180-source.mkv"
        output = root / "tensorrt-result.mkv"
        local_app_data = root / "local-app-data"
        local_app_data.mkdir()
        env = os.environ.copy()
        env["LOCALAPPDATA"] = str(local_app_data)
        env["PATH"] = os.pathsep.join((
            str(SHADOW_RUNTIME), str(SELECTED_RUNTIME), str(CLI.parent),
            str(Path(FFMPEG).parent), env.get("PATH", ""),
        ))

        invoke((FFMPEG, "-v", "error", "-nostdin", "-f", "lavfi", "-i",
                "testsrc2=size=320x180:rate=30:duration=0.2", "-vf",
                "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited",
                "-fps_mode", "cfr", "-c:v", "ffv1", source), env)
        response = invoke((CLI, "interpolate", source, output, "--fps", "60",
                           "--model-dir", MODEL_PACK, "--runtime-dir", SELECTED_RUNTIME,
                           "--encoder", "ffv1"), env)
        events = [json.loads(line) for line in response.stdout.splitlines() if line.startswith("{")]
        if not events or events[-1].get("event") != "interpolation_completed":
            raise AssertionError(f"CLI did not report a completed interpolation: {response.stdout}")
        result = events[-1]
        if result.get("backend") != "onnxruntime_tensorrt_cuda":
            raise AssertionError(f"selected TensorRT backend was not active: {result}")
        if result.get("output_frames") != 12 or result.get("ai_frames", 0) <= 0:
            raise AssertionError(f"unexpected 320x180 output frame counts: {result}")
        probe = invoke((FFPROBE, "-v", "error", "-select_streams", "v:0", "-count_frames",
                        "-show_entries", "stream=nb_read_frames", "-of", "json", output), env)
        output_info = json.loads(probe.stdout)
        if int(output_info["streams"][0]["nb_read_frames"]) != result["output_frames"]:
            raise AssertionError("FFprobe frame count disagrees with the TensorRT result")
        print(json.dumps({"event": "tensorrt_runtime_isolation_passed",
                          "selectedBackend": result["backend"],
                          "shadowRuntimeFirstOnPath": str(SHADOW_RUNTIME),
                          "outputFrames": result["output_frames"],
                          "aiFrames": result["ai_frames"]}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
