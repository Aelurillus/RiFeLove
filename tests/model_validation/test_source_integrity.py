"""Model source integrity checks; runnable by unittest or the model-tools pytest environment."""

import io
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/model_export"))
from acquire_rife import acquire, default_destination, digest
from export_rife import export_pack, inference_state


class ModelSourceIntegrity(unittest.TestCase):
    def test_default_source_directory_tracks_pinned_model_and_rejects_path_escape(self):
        self.assertEqual(default_destination("rife-4.25"), ROOT / "models/weights/rife4.25-reviewed")
        self.assertEqual(default_destination("rife-4.25-lite"), ROOT / "models/weights/rife4.25-lite-reviewed")
        self.assertEqual(default_destination("rife-4.26"), ROOT / "models/weights/rife4.26-reviewed")
        with self.assertRaisesRegex(ValueError, "invalid modelId"):
            default_destination("rife-../../outside")

    def test_only_documented_training_parameters_are_excluded(self):
        self.assertEqual(inference_state({"weight": 1, "teacher.weight": 2, "caltime.bias": 3}, {"weight"}), {"weight": 1})
        with self.assertRaisesRegex(ValueError, "unexpected non-training"):
            inference_state({"weight": 1, "unknown.weight": 2}, {"weight"})

    def test_missing_inference_parameters_are_not_silently_accepted(self):
        with self.assertRaisesRegex(ValueError, "missing inference"):
            inference_state({"teacher.weight": 2}, {"weight"})

    def setUp(self):
        (ROOT / "work").mkdir(exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="model-integrity-", dir=ROOT / "work")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.cache = self.root / "cache"
        self.cache.mkdir()
        payload = b"# trusted source fixture\n"
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("train_log/IFNet.py", payload)
            package.writestr("train_log/__pycache__/IFNet.pyc", b"unapproved bytecode")
            package.writestr("../outside.py", b"unapproved path traversal entry")
        data = archive.getvalue()
        (self.cache / "model.zip").write_bytes(data)
        self.manifest = {
            "archive": {"cacheName": "model.zip", "url": "https://example.invalid/model.zip", "sha256": digest(data)},
            "archiveFiles": [{"entry": "train_log/IFNet.py", "destination": "IFNet.py", "bytes": len(payload), "sha256": digest(payload)}],
            "additionalFiles": [],
        }

    def test_extracts_only_allow_listed_entries(self):
        destination = acquire(self.root / "source", self.cache, self.manifest)
        self.assertEqual({p.name for p in destination.iterdir()}, {"IFNet.py", "source-manifest.json"})
        self.assertFalse((self.root / "outside.py").exists())

    def test_corrupted_archive_is_rejected_before_extraction(self):
        (self.cache / "model.zip").write_bytes(b"tampered archive")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            acquire(self.root / "source", self.cache, self.manifest)
        self.assertFalse((self.root / "source").exists())

    def test_missing_cache_never_downloads_implicitly(self):
        (self.cache / "model.zip").unlink()
        with mock.patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network access")) as network:
            with self.assertRaises(FileNotFoundError):
                acquire(self.root / "source", self.cache, self.manifest)
            network.assert_not_called()

    def test_modified_existing_model_is_not_overwritten(self):
        destination = self.root / "source"
        destination.mkdir()
        (destination / "IFNet.py").write_bytes(b"local modified model")
        with self.assertRaisesRegex(ValueError, "refusing to replace"):
            acquire(destination, self.cache, self.manifest)
        self.assertEqual((destination / "IFNet.py").read_bytes(), b"local modified model")

    def test_export_rejects_modified_code_before_loading_ml_libraries(self):
        destination = acquire(self.root / "source", self.cache, self.manifest)
        marker = self.root / "should-not-execute.txt"
        (destination / "IFNet.py").write_text(f"from pathlib import Path\nPath({str(marker)!r}).write_text('executed')\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            export_pack(destination, self.root / "output-pack", self.manifest)
        self.assertFalse(marker.exists())
        self.assertFalse((self.root / "output-pack").exists())


class UiTranslationIntegrity(unittest.TestCase):
    def test_desktop_translation_catalogs_are_complete_and_preserve_placeholders(self):
        translations = ROOT / "ui/translations"
        catalogs = {
            "en_US": translations / "rifelove_en.ts",
            "zh_TW": translations / "rifelove_zh_TW.ts",
            "ja_JP": translations / "rifelove_ja.ts",
        }
        source_keys = None
        placeholders = re.compile(r"%L?\d+")
        for locale, catalog in catalogs.items():
            tree = ET.parse(catalog)
            self.assertEqual(tree.getroot().get("language"), locale)
            keys = set()
            messages = tree.findall(".//message")
            for context in tree.findall("context"):
                name = context.findtext("name")
                for message in context.findall("message"):
                    source = message.findtext("source", default="")
                    translated = message.find("translation")
                    value = "" if translated is None else "".join(translated.itertext())
                    self.assertTrue(value.strip(), f"{locale} has an empty translation for {name}: {source!r}")
                    self.assertNotEqual(translated.get("type"), "unfinished", f"{locale} has an unfinished translation for {source!r}")
                    self.assertEqual(sorted(placeholders.findall(source)), sorted(placeholders.findall(value)),
                        f"{locale} changed a formatted placeholder in {source!r}")
                    keys.add((name, source))
            self.assertGreaterEqual(len(messages), 360)
            if source_keys is None:
                source_keys = keys
            else:
                self.assertEqual(keys, source_keys, f"{locale} catalog is missing source strings")


if __name__ == "__main__":
    unittest.main()
