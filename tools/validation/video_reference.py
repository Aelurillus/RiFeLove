"""Compare native lossless RIFE video frames with the verified upstream CPU model."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT / "tools/model_export"))
from export_rife import verified_sources, load_network

def run(executable, *arguments, data=None):
    result = subprocess.run([str(executable),*(str(a) for a in arguments)],input=data,capture_output=True,timeout=90)
    if result.returncode:
        raise RuntimeError(f"{Path(executable).name} failed: {result.stderr.decode('utf-8',errors='replace')[-2500:]}")
    return result.stdout

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cli",type=Path); parser.add_argument("ffmpeg",type=Path)
    parser.add_argument("pack",type=Path); parser.add_argument("runtime",type=Path); parser.add_argument("report",type=Path)
    args = parser.parse_args()
    descriptor = json.loads((args.pack / "model.json").read_text(encoding="utf-8"))
    source_manifest_bytes = (args.pack / "source-manifest.json").read_bytes()
    source_manifest = json.loads(source_manifest_bytes)
    manifest_path = ROOT / "models/manifests" / f"{source_manifest['modelId']}.source.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if descriptor["provenance"]["manifestSha256"] != hashlib.sha256(source_manifest_bytes).hexdigest():
        raise AssertionError("Model pack source manifest no longer matches its descriptor")
    if source_manifest["modelId"] != manifest["modelId"]:
        raise AssertionError("Model pack source identity differs from the pinned local manifest")
    source_directory = ROOT / "models/weights" / (manifest["modelId"].replace("rife-", "rife") + "-reviewed")
    sources = verified_sources(source_directory,manifest)
    import numpy as np
    import torch
    torch.set_num_threads(2); torch.set_num_interop_threads(1)
    network,_,_ = load_network(sources)
    with tempfile.TemporaryDirectory(prefix="rife-oracle-",dir=args.cli.resolve().parent) as directory:
        source = Path(directory)/"source.mkv"; output = Path(directory)/"interpolated.mkv"
        color = "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited"
        run(args.ffmpeg,"-v","error","-nostdin","-f","lavfi","-i","testsrc2=size=96x64:rate=5:duration=0.8","-vf",color,"-c:v","ffv1",source)
        job = json.loads(run(args.cli,"interpolate",source,output,"--fps","10","--model-dir",args.pack,"--runtime-dir",args.runtime,"--encoder","ffv1").decode())
        if job["ai_frames"]!=3 or job["output_frames"]!=8:
            raise AssertionError("Oracle fixture did not exercise three real interpolations")
        to_rgb = "scale=flags=bilinear+accurate_rnd:in_color_matrix=bt709:out_color_matrix=bt709:in_range=tv:out_range=pc"
        rgb_bytes = run(args.ffmpeg,"-v","error","-nostdin","-i",source,"-vf",to_rgb,"-pix_fmt","rgb48le","-f","rawvideo","-")
        rgb = np.frombuffer(rgb_bytes,dtype="<u2").reshape(4,64,96,3).astype(np.float32)/np.float32(65535)
        def yuv(path):
            return np.frombuffer(run(args.ffmpeg,"-v","error","-nostdin","-i",path,"-pix_fmt","yuv420p","-f","rawvideo","-"),dtype=np.uint8).reshape(-1,96*64*3//2)
        original,actual = yuv(source),yuv(output)
        if not np.array_equal(actual[[0,2,4,6,7]],original[[0,1,2,3,3]]):
            raise AssertionError("Source-coincident or terminal-hold frames changed in lossless output")
        cases = []
        for pair in range(3):
            tensors = []
            for index in (pair,pair+1):
                planar = np.pad(rgb[index].transpose(2,0,1),((0,0),(0,0),(0,32)),mode="edge")[None]
                tensors.append(torch.from_numpy(np.ascontiguousarray(planar)))
            with torch.inference_mode():
                expected_rgb = network(torch.cat(tensors,1),torch.full((1,1,1,1),0.5),manifest.get("scaleList",[16,8,4,2,1]))[-1][-1].numpy()[0,:,:64,:96].transpose(1,2,0)
            if not np.isfinite(expected_rgb).all(): raise AssertionError("Invalid upstream reference")
            # Match lround(float32 * 65535), including its away-from-zero ties.
            interleaved = np.floor((expected_rgb*np.float32(65535)).astype(np.float64)+0.5).clip(0,65535).astype("<u2")
            to_yuv = "scale=flags=bilinear+accurate_rnd:in_color_matrix=bt709:out_color_matrix=bt709:in_range=pc:out_range=tv"
            expected = np.frombuffer(run(args.ffmpeg,"-v","error","-nostdin","-f","rawvideo","-pixel_format","rgb48le","-video_size","96x64","-i","pipe:0",
                "-vf",to_yuv,"-pix_fmt","yuv420p","-frames:v","1","-f","rawvideo","-",data=interleaved.tobytes()),dtype=np.uint8)
            frame = actual[pair*2+1]
            difference = np.abs(frame.astype(np.int16)-expected.astype(np.int16))
            blend_error = float(np.mean(np.abs(frame.astype(np.float64)-(original[pair].astype(np.float64)+original[pair+1])/2)))
            maximum,mean = int(difference.max()),float(difference.mean())
            if maximum>2 or mean>0.05:
                raise AssertionError(f"Native video/CPU reference mismatch: max={maximum}, mean={mean}")
            if blend_error<=0.05: raise AssertionError("Fixture cannot distinguish neural interpolation from a crossfade")
            cases.append({"output_frame":pair*2+1,"max_code_error":maximum,"mean_code_error":mean,"difference_from_crossfade":blend_error})
        report = {"status":"passed_upstream_video_reference","model_sha256":job["model_sha256"],"source_frames":4,"output_frames":8,
                  "ai_frames":3,"reference":"verified upstream PyTorch CPU plus explicit FFmpeg BT.709 color conversion",
                  "torch":torch.__version__,"cases":cases,"limits":{"maximum_code_error":2,"mean_code_error":0.05},
                  "pending":["full resolutions/content matrix","visual blind review","real-content 10-bit/HDR","TensorRT/FP16"]}
        args.report.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
        print(json.dumps(report))

if __name__=="__main__": main()
