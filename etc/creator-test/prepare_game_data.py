#!/usr/bin/env python3
"""Create a pinned download manifest; upload the four data folders unchanged to its base URL."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from urllib.parse import urlsplit
from prepare_runtime import validate_game_data

DEFAULT_URL = "https://pub-61be48bf4c0f4d208f849304433124c9.r2.dev/"


def validate_manifest(manifest):
    url = urlsplit(manifest.get("baseUrl", ""))
    if manifest.get("format") != 1 or url.scheme != "https" or not url.netloc or not url.path.endswith("/") or url.query or url.fragment:
        raise ValueError("Game data requires an HTTPS base URL ending in / and manifest format 1")
    files = manifest.get("files", [])
    if not files:
        raise ValueError("Game-data manifest is empty")
    seen = set()
    for entry in files:
        path = entry.get("path", "")
        if not re.fullmatch(r"(dbc|maps|vmaps|mmaps)/[A-Za-z0-9_.-]+", path) or path.split('/')[-1] in (".", "..") or path.lower() in seen:
            raise ValueError("Unsafe or duplicate game-data path: " + path)
        seen.add(path.lower())
        if not re.fullmatch(r"[0-9a-f]{64}", entry.get("sha256", "")) or type(entry.get("size")) is not int or not 0 < entry["size"] <= 1024**3:
            raise ValueError("Invalid game-data size or hash: " + path)
    from prepare_runtime import GAME_DATA_PATTERNS
    from fnmatch import fnmatchcase
    for folder, patterns in GAME_DATA_PATTERNS.items():
        for pattern in patterns:
            if not any(fnmatchcase(p, folder + '/' + pattern) for p in seen):
                raise ValueError("Missing game-data group: " + folder + '/' + pattern)


def create_manifest(data, base_url):
    validate_game_data(data)
    files = []
    for folder in ("dbc", "maps", "vmaps", "mmaps"):
        for path in sorted((data / folder).iterdir()):
            if not path.is_file() or path.is_symlink():
                raise ValueError("Expected regular extracted data file: " + str(path))
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            files.append({"path": path.relative_to(data).as_posix(), "size": path.stat().st_size,
                          "sha256": digest.hexdigest()})
    manifest = {"format": 1, "baseUrl": base_url.rstrip('/') + '/', "files": files}
    validate_manifest(manifest)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--base-url", default=DEFAULT_URL)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output exists; use a new manifest filename for a new release")
    manifest = create_manifest(args.data, args.base_url)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(manifest, stream, indent=2)
    print(f"Wrote {len(manifest['files'])} files, {sum(f['size'] for f in manifest['files'])} bytes to {args.output}")

if __name__ == "__main__":
    main()
