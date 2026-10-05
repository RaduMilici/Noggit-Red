"""Offline tests for interrupted portable-runtime downloads and seed preservation."""
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location("prepare_runtime",
    Path(__file__).resolve().parents[2] / "etc/creator-test/prepare_runtime.py")
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)

class Response(io.BytesIO):
    def __init__(self, payload, start, total, declared=None):
        super().__init__(payload)
        self.status = 206
        self.headers = {"Content-Length": str(declared if declared is not None else len(payload)),
                        "Content-Range": f"bytes {start}-{start + len(payload) - 1}/{total}"}

class PackagingTests(unittest.TestCase):
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
                    patch.object(runtime.sys, "argv", ["prepare_runtime", "--output", str(output)]), \
                    patch.object(runtime, "download_database") as download:
                with self.assertRaises(SystemExit) as error:
                    runtime.main()
                self.assertEqual(error.exception.code, 2)
                download.assert_not_called()
            self.assertEqual(seed.read_text(), "preserve")

if __name__ == "__main__":
    unittest.main()
