"""Run real native RIFE video jobs with independent FFmpeg/ffprobe checks."""
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest

CLI, FFMPEG, FFPROBE, PACK, RUNTIME = (Path(p).resolve() for p in sys.argv[1:6])
del sys.argv[1:6]

def invoke(executable, *arguments, expected=0, binary=False):
    result = subprocess.run([str(executable), *(str(a) for a in arguments)],capture_output=True,
                            encoding=None if binary else "utf-8",timeout=60)
    if result.returncode != expected:
        raise AssertionError(f"{executable.name}: expected {expected}, got {result.returncode}: {result.stderr}")
    return result

def probe(path, *arguments):
    return json.loads(invoke(FFPROBE,"-v","error",*arguments,"-of","json",path).stdout)

def frame_bytes(path):
    return invoke(FFMPEG,"-v","error","-nostdin","-i",path,"-map","0:v:0","-pix_fmt","yuv420p","-f","rawvideo","-",binary=True).stdout

class NativeRifePipeline(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="rife-video-",dir=CLI.parent)
        cls.root = Path(cls.temporary.name)
        cls.subtitle = cls.root / "captions.srt"
        cls.subtitle.write_text("1\n00:00:00,000 --> 00:00:00,200\nRifeLove pipeline\n",encoding="utf-8")
        cls.chapters = cls.root / "chapters.txt"
        cls.chapters.write_text(";FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=250\ntitle=Opening\n",encoding="utf-8")
        cls.color = "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited"
        cls.complex = cls.root / "中文 多轨源.mkv"
        invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i","testsrc2=size=96x64:rate=24000/1001:duration=0.25",
               "-f","lavfi","-i","sine=frequency=440:sample_rate=48000:duration=0.25",
               "-f","lavfi","-i","sine=frequency=880:sample_rate=48000:duration=0.25",
               "-i",cls.subtitle,"-f","ffmetadata","-i",cls.chapters,
               "-map","0:v","-map","1:a","-map","2:a","-map","3:s","-map_chapters","4",
               "-vf",cls.color,"-c:v","ffv1","-c:a:0","flac","-c:a:1","aac","-c:s","srt",
               "-metadata:s:a:0","language=zho","-metadata:s:a:1","language=jpn",cls.complex)
        cls.bframes = cls.root / "reordered.mp4"
        invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i","testsrc2=size=96x64:rate=24:duration=0.5",
               "-f","lavfi","-i","sine=frequency=440:sample_rate=48000:duration=0.5",
               "-vf",cls.color,"-c:v","mpeg4","-bf","2","-q:v","3","-c:a","aac",cls.bframes)
        cls.nvdecSource = cls.root / "nvdec-h264.mp4"
        invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i","testsrc2=size=320x180:rate=30:duration=0.5",
               "-vf",cls.color,"-fps_mode","cfr","-c:v","libopenh264","-b:v","512k",cls.nvdecSource)
        cls.bt601TagConflict = cls.root / "bt601-container-vui-conflict.mp4"
        invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i","testsrc2=size=96x64:rate=5:duration=0.6",
               "-f","lavfi","-i","sine=frequency=330:sample_rate=48000:duration=0.6",
               "-map","0:v","-map","1:a",
               "-vf","setparams=color_primaries=bt470bg:color_trc=bt709:colorspace=smpte170m:range=limited",
               "-c:v","libopenh264","-pix_fmt","yuv420p","-c:a","aac",
               "-color_primaries","bt470bg","-color_trc","bt709","-colorspace","smpte170m","-color_range","tv",
               "-bsf:v","h264_metadata=colour_primaries=5:transfer_characteristics=6:matrix_coefficients=5",
               cls.bt601TagConflict)
        cls.vfr = cls.root / "vfr.mkv"
        invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i","testsrc2=size=96x64:rate=30:duration=0.2",
               "-vf",f"setpts='(if(lt(N,2),N,2+(N-2)*2))/(30*TB)+5/TB',{cls.color}","-fps_mode","vfr","-c:v","ffv1",cls.vfr)
        cls.cut = cls.root / "cut.mkv"
        invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i","color=black:size=96x64:rate=4:duration=0.5",
               "-f","lavfi","-i","color=white:size=96x64:rate=4:duration=0.5",
               "-filter_complex",f"[0:v][1:v]concat=n=2:v=1:a=0,{cls.color}","-c:v","ffv1",cls.cut)
        cls.tenBit = cls.root / "ten-bit-bt709.mkv"
        invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i","testsrc2=size=96x64:rate=5:duration=0.8",
               "-vf",cls.color,"-c:v","ffv1","-pix_fmt","yuv420p10le",
               "-color_primaries","bt709","-color_trc","bt709","-colorspace","bt709","-color_range","tv",cls.tenBit)

    @classmethod
    def tearDownClass(cls): cls.temporary.cleanup()

    def run_job(self, source, output, rate="60", *options, expected=0):
        response = invoke(CLI,"interpolate",source,output,"--fps",rate,"--model-dir",PACK,"--runtime-dir",RUNTIME,*options,expected=expected)
        events = [json.loads(line) for line in response.stdout.splitlines()]
        return events[-1] if events else None, events

    def verify_timing(self, output, result, allow_quantized_declared_end=False):
        records = probe(output,"-select_streams","v:0","-show_frames","-show_entries","frame=best_effort_timestamp")
        stream = probe(output,"-select_streams","v:0","-show_streams")["streams"][0]
        base, origin, rate, end = map(Fraction,(stream["time_base"],result["first_pts"],result["target_fps"],result["end_exclusive"]))
        expected_count = math.ceil((end-origin)*rate)
        self.assertEqual(len(records["frames"]),expected_count)
        self.assertEqual(result["output_frames"],expected_count)
        times = [int(f["best_effort_timestamp"])*base for f in records["frames"]]
        for index,time in enumerate(times): self.assertLessEqual(abs(time-origin-Fraction(index)/rate),base/2)
        self.assertTrue(all(b>a for a,b in zip(times,times[1:])))
        self.assertLessEqual(abs(Fraction(result["end_error"])),base)
        if allow_quantized_declared_end:
            self.assertLessEqual(abs(Fraction(result["declared_end_error"])),base/2)
        else:
            self.assertEqual(Fraction(result["declared_end_error"]),0)
        self.assertEqual(result["ai_frames"]+result["original_frames"]+result["held_frames"],expected_count)

    def test_fractional_rate_preserves_multiaudio_subtitle_chapter_and_samples(self):
        output = self.root / "中文 60000-1001.mkv"
        original_hash = hashlib.sha256(self.complex.read_bytes()).digest()
        result,_ = self.run_job(self.complex,output,"60000/1001","--encoder","ffv1","--progress")
        self.assertIn(result["backend"],("onnxruntime_cuda","onnxruntime_tensorrt_cuda"))
        if result["backend"] == "onnxruntime_cuda": self.assertTrue(result["inference_fallback_reason"])
        else: self.assertFalse(result["inference_fallback_reason"])
        self.assertGreater(result["ai_frames"],0)
        self.verify_timing(output,result)
        info = probe(output,"-show_streams","-show_chapters","-show_format")
        self.assertEqual([s["codec_type"] for s in info["streams"]],["video","audio","audio","subtitle"])
        self.assertEqual([s.get("tags",{}).get("language") for s in info["streams"][1:3]],["zho","jpn"])
        self.assertEqual(info["chapters"][0]["tags"]["title"],"Opening")
        for selector in ("a:0","a:1","s:0"):
            before = probe(self.complex,"-select_streams",selector,"-show_packets","-show_data_hash","sha256","-show_entries","packet=pts,dts,data_hash")
            after = probe(output,"-select_streams",selector,"-show_packets","-show_data_hash","sha256","-show_entries","packet=pts,dts,data_hash")
            self.assertEqual(before,after)
        for selector in ("0:a:0","0:a:1"):
            before = invoke(FFMPEG,"-v","error","-i",self.complex,"-map",selector,"-f","s16le","-",binary=True).stdout
            after = invoke(FFMPEG,"-v","error","-i",output,"-map",selector,"-f","s16le","-",binary=True).stdout
            self.assertEqual(before,after)
        self.assertEqual(hashlib.sha256(self.complex.read_bytes()).digest(),original_hash)

    def test_coarse_container_clock_reuses_source_frames_that_round_to_output_ticks(self):
        source = self.root / "quantized-source-30.mkv"
        output = self.root / "quantized-result-60.mkv"
        invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i","testsrc2=size=96x64:rate=30:duration=0.8",
               "-vf",self.color,"-fps_mode","cfr","-c:v","ffv1",source)
        source_info = probe(source,"-select_streams","v:0","-show_streams","-show_frames",
                            "-show_entries","stream=time_base:frame=best_effort_timestamp")
        self.assertEqual(source_info["streams"][0]["time_base"],"1/1000")
        result,_ = self.run_job(source,output,"60","--encoder","ffv1")
        self.assertEqual((result["source_frames"],result["output_frames"]),(24,48))
        self.assertEqual((result["original_frames"],result["ai_frames"],result["held_frames"]),(24,23,1))
        self.verify_timing(output,result)

        output_info = probe(output,"-select_streams","v:0","-show_streams","-show_frames",
                            "-show_entries","stream=time_base:frame=best_effort_timestamp")
        source_base = Fraction(source_info["streams"][0]["time_base"])
        output_base = Fraction(output_info["streams"][0]["time_base"])
        source_times = [int(frame["best_effort_timestamp"]) * source_base for frame in source_info["frames"]]
        output_times = [int(frame["best_effort_timestamp"]) * output_base for frame in output_info["frames"]]
        output_indexes = {timestamp:index for index,timestamp in enumerate(output_times)}
        source_pixels,output_pixels = frame_bytes(source),frame_bytes(output)
        frame_size = 96 * 64 * 3 // 2
        for source_index,timestamp in enumerate(source_times):
            self.assertIn(timestamp,output_indexes)
            output_index = output_indexes[timestamp]
            self.assertEqual(source_pixels[source_index*frame_size:(source_index+1)*frame_size],
                             output_pixels[output_index*frame_size:(output_index+1)*frame_size])

    def test_vfr_noninteger_conversion_preserves_nonzero_origin_and_end(self):
        output = self.root / "vfr-144.mkv"
        result,_ = self.run_job(self.vfr,output,"144","--encoder","ffv1")
        self.verify_timing(output,result); self.assertEqual(Fraction(result["first_pts"]),5)
        self.assertGreater(result["ai_frames"],0)

    def test_360fps_target_uses_exact_timeline_and_real_rife_frames(self):
        output = self.root / "rife-360.mkv"
        result,_ = self.run_job(self.complex,output,"360","--encoder","ffv1")
        self.assertEqual(result["target_fps"],"360/1")
        self.assertGreater(result["output_frames"],70)
        self.assertGreater(result["ai_frames"],0)
        self.verify_timing(output,result)

    def test_480fps_short_video_preserves_aac_and_rounded_final_boundary(self):
        source = self.root / "high-fps-short-with-audio.mp4"
        output = self.root / "high-fps-short-with-audio.mkv"
        invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i",
               "testsrc2=size=96x64:rate=32:duration=0.0625",
               "-f","lavfi","-i","sine=frequency=440:sample_rate=48000:duration=0.0625",
               "-c:v","libopenh264","-pix_fmt","yuv420p",
               "-bsf:v","h264_metadata=colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1",
               "-c:a","aac",source)
        result,_ = self.run_job(source,output,"480","--encoder","ffv1","--backend","cuda")
        self.assertEqual(result["output_frames"],30)
        self.assertEqual(Fraction(result["end_exclusive"]),Fraction(1,16))
        self.assertGreater(result["ai_frames"],0)
        self.verify_timing(output,result,allow_quantized_declared_end=True)
        self.assert_audio_samples_equal(source,output)

    def test_cut_and_duplicate_protection_never_generates_a_crossfade(self):
        output = self.root / "cut-8.mkv"
        result,_ = self.run_job(self.cut,output,"8","--encoder","ffv1")
        self.verify_timing(output,result)
        self.assertEqual(result["ai_frames"],0); self.assertEqual(result["scene_cuts"],1); self.assertEqual(result["duplicate_pairs"],2)
        source, actual = frame_bytes(self.cut),frame_bytes(output)
        size = 96*64*3//2
        for index in range(8): self.assertEqual(actual[index*size:(index+1)*size],source[(0 if index<4 else 2)*size:(1 if index<4 else 3)*size])

    def test_desktop_exported_local_profile_drives_direct_interpolation(self):
        profile_path = self.root / "本机 配置.json"
        profile_path.write_text(json.dumps({
            "format": "rifelove-local-profile", "version": 1,
            "profile": {
                "name": "本机 SDR", "rate": "60", "modelUrl": PACK.as_uri(),
                "runtimeUrl": RUNTIME.as_uri(), "outputUrl": "", "encoder": "auto",
                "container": "mkv", "bitrateKbps": 8000, "qualityMode": "bitrate",
                "quality": 32, "gopSize": 120,
            },
        }, ensure_ascii=False), encoding="utf-8")
        output = self.root / "profile-60.mkv"
        response = invoke(CLI, "interpolate", self.nvdecSource, output, "--profile", profile_path, "--encoder", "ffv1", "--progress")
        events = [json.loads(line) for line in response.stdout.splitlines()]
        self.assertEqual(events[-1]["event"], "interpolation_completed")
        self.assertEqual(events[-1]["target_fps"], "60/1")
        self.assertGreater(events[-1]["ai_frames"], 0)
        self.assertEqual(events[-1]["source_decode_acceleration"], "cuda")
        self.assertEqual(events[-1]["source_decode_fallback_reason"], "")
        self.verify_timing(output, events[-1])
        source_pixels, output_pixels = frame_bytes(self.nvdecSource), frame_bytes(output)
        frame_size = 320 * 180 * 3 // 2
        self.assertEqual(len(source_pixels), 15 * frame_size)
        self.assertEqual(len(output_pixels), 30 * frame_size)
        for index in range(15):
            self.assertEqual(source_pixels[index * frame_size:(index + 1) * frame_size],
                             output_pixels[index * 2 * frame_size:(index * 2 + 1) * frame_size])

    def test_auto_h264_fallback_and_fractional_mp4_timing(self):
        output = self.root / "auto.mp4"
        report_path = self.root / "auto-report.json"
        result,events = self.run_job(self.bframes,output,"60000/1001","--report",report_path,"--progress")
        self.verify_timing(output,result)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report["schemaVersion"],1)
        self.assertEqual(report["source"]["path"],str(self.bframes))
        self.assertEqual(report["output"]["path"],str(output))
        self.assertEqual(report["output"]["targetFrameRate"],"60000/1001")
        self.assertEqual(report["output"]["encoder"],result["encoder"])
        self.assertEqual(report["pipelineResult"]["outputFrames"],result["output_frames"])
        self.assertEqual(report["source"]["recoveredTimestamps"],0)
        self.assertEqual(result["recovered_timestamps"],0)
        self.assertFalse(report["qualityDiagnostics"]["perceptualMetricsMeasured"])
        self.assertIn(result["encoder"],("h264_nvenc","libopenh264"))
        encoded = probe(output,"-select_streams","v:0","-show_streams")["streams"][0]
        self.assertIn(result["encoder"],encoded["tags"]["encoder"])
        if result["encoder"]=="libopenh264":
            self.assertTrue(result["encoder_fallback_reason"]); self.assertTrue(any(e.get("stage")=="encoder_fallback" for e in events))

    def test_bt601_container_and_h264_vui_tag_conflict_runs_with_stream_tags_preserved(self):
        source_tags = probe(self.bt601TagConflict,"-select_streams","v:0","-show_streams","-show_frames",
                            "-read_intervals","%+#1","-show_entries",
                            "stream=color_primaries,color_transfer,color_space:frame=color_primaries,color_transfer,color_space")
        stream = source_tags["streams"][0]
        frame = source_tags["frames"][0]
        self.assertEqual((stream["color_primaries"],stream["color_transfer"],stream["color_space"]),
                         ("bt470bg","bt709","smpte170m"))
        self.assertEqual((frame["color_primaries"],frame["color_transfer"],frame["color_space"]),
                         ("bt470bg","smpte170m","bt470bg"))

        output = self.root / "bt601-tag-conflict-output.mkv"
        result,_ = self.run_job(self.bt601TagConflict,output,"10","--encoder","ffv1")
        self.assertEqual(result["source_decode_acceleration"],"cuda")
        self.verify_timing(output,result)
        self.assertGreater(result["ai_frames"],0)
        output_video = probe(output,"-select_streams","v:0","-show_streams")["streams"][0]
        self.assertEqual((output_video["color_primaries"],output_video["color_transfer"],output_video["color_space"]),
                         ("bt470bg","bt709","smpte170m"))
        self.assert_audio_samples_equal(self.bt601TagConflict,output)

    def test_ten_bit_sdr_model_profile_preserves_output_depth_and_color_tags(self):
        input_video = probe(self.tenBit,"-select_streams","v:0","-show_streams")["streams"][0]
        self.assertEqual(input_video["pix_fmt"],"yuv420p10le")
        self.assertEqual((input_video["color_primaries"],input_video["color_transfer"],input_video["color_space"],input_video["color_range"]),
                         ("bt709","bt709","bt709","tv"))
        output = self.root / "ten-bit-rife.mkv"
        result,_ = self.run_job(self.tenBit,output,"10","--encoder","auto")
        self.assertNotEqual(result["encoder"],"h264_nvenc")
        self.assertIn("encoder does not support the requested pixel format",result["encoder_fallback_reason"])
        self.verify_timing(output,result)
        self.assertGreater(result["ai_frames"],0)
        output_video = probe(output,"-select_streams","v:0","-show_streams")["streams"][0]
        self.assertEqual(output_video["pix_fmt"],"yuv420p10le")
        self.assertEqual((output_video["color_primaries"],output_video["color_transfer"],output_video["color_space"],output_video["color_range"]),
                         ("bt709","bt709","bt709","tv"))

    def test_hevc_and_av1_real_interpolation_and_audio_copy(self):
        for encoder,extension,options in (
            ("libkvazaar","mp4",[]),
            ("libaom-av1","mkv",["--option","cpu-used=8","--option","threads=2"]),
        ):
            with self.subTest(encoder=encoder):
                output = self.root / f"{encoder}.{extension}"
                result,_ = self.run_job(self.bframes,output,"60","--encoder",encoder,*options)
                self.verify_timing(output,result); self.assertGreater(result["ai_frames"],0)
                self.assert_audio_samples_equal(self.bframes,output)

    def assert_audio_samples_equal(self, source, output):
        before = invoke(FFMPEG,"-v","error","-i",source,"-map","0:a:0","-f","s16le","-",binary=True).stdout
        after = invoke(FFMPEG,"-v","error","-i",output,"-map","0:a:0","-f","s16le","-",binary=True).stdout
        self.assertEqual(before,after)

    def test_mp4_aac_priming_survives_mkv_interpolation_and_remux(self):
        output = self.root / "priming.mkv"
        result,_ = self.run_job(self.bframes,output,"60","--encoder","ffv1")
        self.verify_timing(output,result); self.assert_audio_samples_equal(self.bframes,output)
        remuxed = self.root / "priming-copy.mkv"
        invoke(CLI,"remux",self.bframes,remuxed)
        self.assert_audio_samples_equal(self.bframes,remuxed)

    def test_short_h264_aac_track_ends_do_not_inherit_container_duration(self):
        # With only three H.264 frames FFmpeg may populate AVStream.duration
        # from the longer container duration. The Matroska video DURATION tag
        # and decoded final frame still end at exactly 3/4 s.
        for source_extension in ("mp4","mkv"):
            for audio_duration in ("0.75","0.9"):
                source = self.root / f"short-{source_extension}-{audio_duration}.{source_extension}"
                invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i",
                       "color=black:size=192x128:rate=4:duration=0.75",
                       "-f","lavfi","-i",f"sine=frequency=440:sample_rate=48000:duration={audio_duration}",
                       "-vf",self.color,"-c:v","libopenh264","-pix_fmt","yuv420p","-c:a","aac",
                       "-avoid_negative_ts","disabled",source)
                for target_extension in ("mkv","mp4","mov"):
                    with self.subTest(source=source_extension,audio_duration=audio_duration,target=target_extension):
                        output = self.root / f"short-{source_extension}-{audio_duration}-4.{target_extension}"
                        result,_ = self.run_job(source,output,"4","--encoder","libopenh264","--backend","cuda")
                        self.verify_timing(output,result)
                        self.assertEqual(result["output_frames"],3)
                        self.assertEqual(Fraction(result["end_exclusive"]),Fraction(3,4))
                        self.assert_audio_samples_equal(source,output)
                        before = probe(source,"-select_streams","a:0","-show_packets","-show_data_hash","sha256",
                                       "-show_entries","packet=data_hash")["packets"]
                        after = probe(output,"-select_streams","a:0","-show_packets","-show_data_hash","sha256",
                                      "-show_entries","packet=data_hash")["packets"]
                        # ffprobe also emits container-derived skip-sample side data,
                        # even when only data_hash was requested. MP4 and Matroska
                        # can represent priming differently; decoded PCM equality
                        # above checks the audible result independently.
                        self.assertEqual([packet["data_hash"] for packet in before],
                                         [packet["data_hash"] for packet in after])

    def test_fractional_h264_aac_tail_keeps_separate_audio_and_video_boundaries(self):
        source = self.root / "fractional-h264-aac-tail-source.mkv"
        invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i",
               "testsrc2=size=96x64:rate=24000/1001:duration=0.25",
               "-f","lavfi","-i","sine=frequency=440:sample_rate=48000:duration=0.4",
               "-vf",self.color,"-c:v","libopenh264","-pix_fmt","yuv420p","-c:a","aac",
               "-avoid_negative_ts","disabled",source)
        source_video = probe(source,"-select_streams","v:0","-show_streams")["streams"][0]
        source_end = Fraction(source_video["tags"]["DURATION"].split(":")[-1])
        for extension in ("mkv","mp4","mov"):
            with self.subTest(container=extension):
                output = self.root / f"fractional-h264-aac-tail.{extension}"
                result,_ = self.run_job(source,output,"60000/1001","--encoder","libopenh264","--backend","cuda")
                self.verify_timing(output,result)
                self.assertEqual(Fraction(result["end_exclusive"]),source_end)
                self.assertGreater(result["ai_frames"],0)
                self.assert_audio_samples_equal(source,output)

    def test_contradictory_matroska_video_track_end_still_rejects_publication(self):
        source = self.root / "contradictory-video-end.mkv"
        invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i",
               "color=black:size=192x128:rate=4:duration=0.75",
               "-vf",self.color,"-c:v","libopenh264",source)
        original = source.read_bytes()
        self.assertIn(b"00:00:00.750000000",original)
        source.write_bytes(original.replace(b"00:00:00.750000000",b"00:00:00.650000000"))
        output = self.root / "rejected-contradictory-end.mkv"
        response = invoke(CLI,"interpolate",source,output,"--fps","4","--model-dir",PACK,
                          "--runtime-dir",RUNTIME,"--encoder","libopenh264","--backend","cuda",expected=2)
        self.assertIn("Declared video end contradicts",response.stderr)
        self.assertFalse(output.exists())
        self.assertEqual(list(self.root.glob("*.partial")),[])

    def test_bframe_source_decodes_and_writes_h264_mov(self):
        output = self.root / "reordered.mov"
        result,_ = self.run_job(self.bframes,output,"60","--encoder","libopenh264")
        self.verify_timing(output,result); self.assertGreater(result["ai_frames"],0)

    def test_bframe_output_is_drained_in_mkv(self):
        output = self.root / "reordered-encoded.mkv"
        result,_ = self.run_job(self.bframes,output,"60","--encoder","mpeg4","--bframes","2")
        self.verify_timing(output,result)

    def test_unrepresentable_color_metadata_prevents_mov_publication(self):
        output = self.root / "unsupported-mpeg4.mov"
        response = invoke(CLI,"interpolate",self.bframes,output,"--fps","60","--model-dir",PACK,"--runtime-dir",RUNTIME,"--encoder","mpeg4","--bframes","2",expected=2)
        self.assertIn("changed color metadata",response.stderr)
        self.assertFalse(output.exists()); self.assertEqual(list(self.root.glob("*.partial")),[])

    def test_existing_output_is_preserved_and_incompatible_container_leaves_no_partial(self):
        output = self.root / "existing.mkv"; output.write_bytes(b"existing data")
        self.run_job(self.complex,output,"60","--encoder","ffv1",expected=2)
        self.assertEqual(output.read_bytes(),b"existing data")
        failed = self.root / "incompatible.mp4"
        self.run_job(self.complex,failed,"60","--encoder","libopenh264",expected=2)
        self.assertFalse(failed.exists()); self.assertEqual(list(self.root.glob("*.partial")),[])

    @unittest.skipUnless(os.name=="nt","Windows console cancellation")
    def test_ctrl_break_cancels_processing_and_removes_partial_output(self):
        output = self.root / "cancelled.mkv"
        source = self.root / "longer-cancel-source.mkv"
        invoke(FFMPEG,"-v","error","-nostdin","-f","lavfi","-i","testsrc2=size=96x64:rate=24:duration=2",
               "-vf",self.color,"-c:v","ffv1",source)
        command = [str(CLI),"interpolate",str(source),str(output),"--fps","240","--model-dir",str(PACK),"--runtime-dir",str(RUNTIME),"--encoder","ffv1","--progress"]
        process = subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,encoding="utf-8",creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        try:
            for line in process.stdout:
                event = json.loads(line)
                if event.get("stage")=="processing" and event.get("output_frames",0)>=5:
                    process.send_signal(signal.CTRL_BREAK_EVENT); break
            remainder,_ = process.communicate(timeout=30)
            self.assertEqual(process.returncode,130,remainder)
            self.assertIn('"interpolation_cancelled"',remainder)
            self.assertFalse(output.exists()); self.assertEqual(list(self.root.glob("*.partial")),[])
        finally:
            if process.poll() is None: process.kill(); process.wait()

if __name__=="__main__": unittest.main()
