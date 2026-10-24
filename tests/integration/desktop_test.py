"""Generate local media and run the real QML/controller/native-GPU workflow."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import shutil

exe, ffmpeg, ffprobe, pack, runtime, qml, reports = sys.argv[1:]
reports = Path(reports)
reports.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="rifelove-desktop-") as folder:
    root = Path(folder)
    source = root / "本地源 视频.mkv"
    output = root / "补帧输出.mkv"
    subprocess.run([ffmpeg, "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=5:duration=0.6",
                    "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=0.6",
                    "-vf", "setparams=color_primaries=bt470bg:color_trc=bt709:colorspace=smpte170m:range=limited",
                    "-c:v", "ffv1", "-pix_fmt", "yuv420p", "-c:a", "flac", str(source)], check=True)
    batch = root / "queue-input"
    nested = batch / "nested"
    nested.mkdir(parents=True)
    shutil.copy2(source, nested / source.name)
    shutil.copytree(pack, root / "local-model-copy")
    shutil.copytree(pack, root / "corrupt-model-copy")
    with (root / "corrupt-model-copy" / "model.onnx").open("ab") as corrupt_model:
        corrupt_model.write(b"integrity-check regression fixture")
    hdr = batch / "unsupported-hdr.mkv"
    subprocess.run([ffmpeg, "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=5:duration=0.6",
                    "-vf", "setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=limited",
                    "-c:v", "ffv1", "-pix_fmt", "yuv420p", str(hdr)], check=True)
    fingerprint = hashlib.sha256(source.read_bytes()).hexdigest()
    subprocess.run([exe, str(source), str(output), pack, runtime, qml, str(reports / "desktop-workflow.png"),
                    str(reports / "desktop-workflow.json")], check=True, timeout=180)
    result = json.loads(subprocess.check_output([ffprobe, "-v", "error", "-count_frames", "-show_streams", "-of", "json", str(output)]))
    video = next(stream for stream in result["streams"] if stream["codec_type"] == "video")
    assert int(video["nb_read_frames"]) == 36
    assert video["color_primaries"] == "bt470bg"
    assert video["color_transfer"] == "bt709"
    assert video["color_space"] == "smpte170m"
    assert video["color_range"] == "tv"
    audio = next(stream for stream in result["streams"] if stream["codec_type"] == "audio")
    assert audio["codec_name"] == "flac"
    def pcm(path):
        return subprocess.check_output([ffmpeg, "-v", "error", "-i", str(path), "-map", "0:a:0", "-f", "s16le", "-"])
    assert pcm(source) == pcm(output), "Desktop output changed decoded audio samples"
    queue_files = sorted((root / "queue-output").glob("*_RifeLove*.mkv"))
    assert len(queue_files) == 3, f"Expected three completed collision-safe queue outputs, got {queue_files}"
    assert not (root / "queue-output" / "cancel-source_RifeLove.mkv").exists(), "Cancelled queued task published an output"
    for queued in queue_files:
        result = json.loads(subprocess.check_output([ffprobe, "-v", "error", "-count_frames", "-show_streams", "-of", "json", str(queued)]))
        video = next(stream for stream in result["streams"] if stream["codec_type"] == "video")
        audio = next(stream for stream in result["streams"] if stream["codec_type"] == "audio")
        assert int(video["nb_read_frames"]) == 36
        assert audio["codec_name"] == "flac"
        assert pcm(source) == pcm(queued), "Queue output changed decoded audio samples"
    assert hashlib.sha256(source.read_bytes()).hexdigest() == fingerprint
    assert hashlib.sha256(source.read_bytes()).hexdigest() == fingerprint
    assert json.loads((reports / "desktop-workflow.json").read_text())["success"]
print("Independent desktop output frame count, decoded audio and source integrity passed")
