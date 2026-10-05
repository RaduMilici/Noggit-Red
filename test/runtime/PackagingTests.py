"""Offline tests for interrupted portable-runtime downloads and seed preservation."""
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location("prepare_runtime",
    Path(__file__).resolve().parents[2] / "etc/creator-test/prepare_runtime.py")
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)
sys.path.insert(0, str(Path(runtime.__file__).parent))
import prepare_game_data as game_data

class Response(io.BytesIO):
    def __init__(self, payload, start, total, declared=None):
        super().__init__(payload)
        self.status = 206
        self.headers = {"Content-Length": str(declared if declared is not None else len(payload)),
                        "Content-Range": f"bytes {start}-{start + len(payload) - 1}/{total}"}

class PackagingTests(unittest.TestCase):
    def populate_data(self, root):
        for folder, patterns in runtime.GAME_DATA_PATTERNS.items():
            (root / folder).mkdir()
            for pattern in patterns:
                (root / folder / pattern.replace("*", "fixture")).write_bytes(b"fixture")

    def test_game_data_requires_tiles_and_nonempty_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.populate_data(root)
            runtime.validate_game_data(root)
            tile = root / "mmaps/fixture.mmtile"
            tile.write_bytes(b"")
            with self.assertRaisesRegex(RuntimeError, "mmaps"):
                runtime.validate_game_data(root)
            tile.unlink()
            with self.assertRaisesRegex(RuntimeError, "mmaps"):
                runtime.validate_game_data(root)

    def test_missing_data_fails_before_network(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(runtime.sys, "platform", "win32"), \
                    patch.object(runtime.platform, "machine", return_value="AMD64"), \
                    patch.object(runtime.sys, "argv", ["prepare_runtime", "--output", str(root / "out"),
                        "--data", str(root / "missing"), "--ssh", "unused"]), \
                    patch.object(runtime, "checkout") as checkout, \
                    patch.object(runtime, "download_database") as download:
                with self.assertRaisesRegex(RuntimeError, "Incomplete extracted game data"):
                    runtime.main()
                checkout.assert_not_called()
                download.assert_not_called()
                self.assertFalse((root / "out").exists())

    def test_linux_requires_wine_and_ssh_keyscan(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "ssh").touch()
            with patch.object(runtime.sys, "platform", "linux"):
                with self.assertRaisesRegex(RuntimeError, "ssh-keyscan"):
                    runtime.validate_tools(root, None)
                (root / "ssh-keyscan").touch()
                with self.assertRaisesRegex(RuntimeError, "Wine"):
                    runtime.validate_tools(root, None)

    def test_manifest_hashes_files_and_rejects_unsafe_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.populate_data(root)
            manifest = game_data.create_manifest(root, "https://assets.example/release-1/")
            self.assertEqual(manifest["files"][0]["sha256"], hashlib.sha256(b"fixture").hexdigest())
            manifest["files"][0]["path"] = "dbc/../outside"
            with self.assertRaisesRegex(ValueError, "Unsafe"):
                game_data.validate_manifest(manifest)

    def test_manifest_requires_all_data_groups_and_https(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.populate_data(root)
            manifest = game_data.create_manifest(root, "https://assets.example/")
            manifest["files"] = [f for f in manifest["files"] if not f["path"].endswith(".mmtile")]
            with self.assertRaisesRegex(ValueError, "Missing game-data group"):
                game_data.validate_manifest(manifest)
            manifest["baseUrl"] = "http://assets.example/"
            with self.assertRaisesRegex(ValueError, "HTTPS"):
                game_data.validate_manifest(manifest)

    def archive(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr("mariadb-fixture/bin/mariadbd.exe", b"fixture")
        return buffer.getvalue()

    def test_interrupted_download_resumes_without_installing_partial_archive(self):
        payload = self.archive()
        offsets = []
        def open_request(request, timeout):
            start = int(request.get_header("Range").split("=")[1].split("-")[0])
            offsets.append(start)
            if len(offsets) == 1:
                return Response(payload[start:start + 11], start, len(payload), declared=len(payload) - start)
            return Response(payload[start:], start, len(payload))
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            (cache / "fixture.download").write_bytes(payload[:7])
            package = {"win32": ("fixture.zip", hashlib.sha256(payload).hexdigest())}
            with patch.object(runtime.sys, "platform", "win32"), patch.object(runtime, "MARIADB", package), \
                    patch.object(runtime.urllib.request, "urlopen", side_effect=open_request), \
                    patch.object(runtime.time, "sleep"):
                result = runtime.download_database(cache)
            self.assertEqual(offsets, [7, 18])
            self.assertEqual((result / "bin/mariadbd.exe").read_bytes(), b"fixture")
            self.assertFalse((cache / "fixture.download").exists())

    def test_checksum_failure_does_not_extract_archive(self):
        payload = self.archive()
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            (cache / "fixture.zip").write_bytes(payload)
            with patch.object(runtime.sys, "platform", "win32"), \
                    patch.object(runtime, "MARIADB", {"win32": ("fixture.zip", "0" * 64)}):
                with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
                    runtime.download_database(cache)
            self.assertFalse((cache / "mariadb-extracted").exists())

    def test_existing_output_is_preserved_before_network_or_server_work(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            seed = output / "designer-data"
            seed.write_text("preserve")
            with patch.object(runtime.sys, "platform", "win32"), \
                    patch.object(runtime.platform, "machine", return_value="AMD64"), \
                    patch.object(runtime.sys, "argv", ["prepare_runtime", "--output", str(output), "--data", "unused", "--ssh", "unused"]), \
                    patch.object(runtime, "download_database") as download:
                with self.assertRaises(SystemExit) as error:
                    runtime.main()
                self.assertEqual(error.exception.code, 2)
                download.assert_not_called()
            self.assertEqual(seed.read_text(), "preserve")

if __name__ == "__main__":
    unittest.main()
