#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import quote


WEBFLASH_BIN = "lofi_cardputer-webflash.bin"
WEBFLASH_MANIFEST = "webflash-manifest.json"
CHECKSUMS = "SHA256SUMS.txt"
README = "README.txt"


@dataclass(frozen=True)
class FlashPart:
    offset: int
    source: Path
    archive_name: str


@dataclass(frozen=True)
class PackageResult:
    package_name: str
    asset_names: list[str]
    release_asset_dir: Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL webflash package: {message}")


def read_json(path: Path) -> dict[str, Any]:
    require(path.exists(), f"missing {path}")
    data = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(data, dict), f"{path} must contain a JSON object")
    return data


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def asset_url(repo: str, tag: str, asset_name: str) -> str:
    return (
        f"https://github.com/{repo}/releases/download/"
        f"{quote(tag, safe='')}/{quote(asset_name, safe='')}"
    )


def normalize_version(tag: str) -> str:
    version = tag.strip()
    require(version, "release tag must not be empty")
    return version.replace("/", "-")


def load_flash_parts(build_dir: Path, firmware_name: str) -> tuple[dict[str, str], list[FlashPart]]:
    flasher = read_json(build_dir / "flasher_args.json")
    settings = flasher.get("flash_settings", {})
    require(settings.get("flash_size") == "8MB", "unexpected flash size")
    require(settings.get("flash_mode") == "dio", "unexpected flash mode")
    require(settings.get("flash_freq") == "80m", "unexpected flash frequency")
    require(flasher.get("extra_esptool_args", {}).get("chip") == "esp32s3", "unexpected chip")

    flash_files = flasher.get("flash_files", {})
    require(isinstance(flash_files, dict), "flash_files must be a JSON object")
    required = {
        0x0: "bootloader/bootloader.bin",
        0x8000: "partition_table/partition-table.bin",
        0x10000: f"{firmware_name}.bin",
    }

    parts: list[FlashPart] = []
    for offset, relative in required.items():
        found = flash_files.get(hex(offset)) or flash_files.get(f"0x{offset:x}")
        require(found == relative, f"unexpected file at 0x{offset:x}: {found!r}")
        source = build_dir / relative
        require(source.exists(), f"missing flash part {source}")
        parts.append(FlashPart(offset=offset, source=source, archive_name=Path(relative).name))

    return settings, parts


def raw_merge(parts: list[FlashPart], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as handle:
        cursor = 0
        for part in sorted(parts, key=lambda item: item.offset):
            require(part.offset >= cursor, f"overlapping flash part at 0x{part.offset:x}")
            handle.write(b"\xff" * (part.offset - cursor))
            data = part.source.read_bytes()
            handle.write(data)
            cursor = part.offset + len(data)


def merge_with_esptool(
    esptool: str,
    settings: dict[str, str],
    parts: list[FlashPart],
    output: Path,
) -> None:
    command = [
        esptool,
        "--chip",
        "esp32s3",
        "merge_bin",
        "-o",
        str(output),
        "--flash_mode",
        settings["flash_mode"],
        "--flash_freq",
        settings["flash_freq"],
        "--flash_size",
        settings["flash_size"],
    ]
    for part in sorted(parts, key=lambda item: item.offset):
        command.extend([f"0x{part.offset:x}", str(part.source)])
    subprocess.run(command, check=True)


def build_manifest(tag: str, repo: str | None, asset_base_url: str | None) -> dict[str, Any]:
    if asset_base_url:
        bin_path = f"{asset_base_url.rstrip('/')}/{WEBFLASH_BIN}"
    elif repo:
        bin_path = asset_url(repo, tag, WEBFLASH_BIN)
    else:
        bin_path = WEBFLASH_BIN
    return {
        "name": "Cardputer Adv Lo-Fi Player",
        "version": tag,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "parts": [
                    {
                        "path": bin_path,
                        "offset": 0,
                    }
                ],
            }
        ],
    }


def write_zip(package_dir: Path, zip_path: Path) -> None:
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(package_dir.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(package_dir.parent))


def write_checksums(paths: list[Path], output: Path) -> None:
    lines = [f"{sha256(path)}  {path.name}" for path in sorted(paths, key=lambda item: item.name)]
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def copy_optional(build_dir: Path, package_dir: Path, names: list[str]) -> None:
    for name in names:
        source = build_dir / name
        if source.exists():
            shutil.copy2(source, package_dir / source.name)


def package_webflash(args: argparse.Namespace) -> PackageResult:
    build_dir = args.build_dir
    dist_dir = args.dist_dir
    tag = args.release_tag
    package_version = normalize_version(tag)
    package_name = f"{args.firmware_name}-{package_version}.zip"
    package_dir = dist_dir / f"{args.firmware_name}-{package_version}"
    release_asset_dir = dist_dir / "release-assets"

    if package_dir.exists():
        shutil.rmtree(package_dir)
    if release_asset_dir.exists():
        shutil.rmtree(release_asset_dir)
    package_dir.mkdir(parents=True)
    release_asset_dir.mkdir(parents=True)

    settings, parts = load_flash_parts(build_dir, args.firmware_name)

    for part in parts:
        target = package_dir / part.archive_name
        if part.source.name == "partition-table.bin":
            target = package_dir / "partition-table.bin"
        shutil.copy2(part.source, target)

    copy_optional(build_dir, package_dir, ["flash_args", "flasher_args.json", "project_description.json"])

    (package_dir / README).write_text(
        "\n".join(
            [
                "Cardputer Adv Lo-Fi Player 固件包",
                "",
                f"构建来源: {args.repo or 'unknown'}@{args.commit or 'unknown'}",
                f"Release tag: {tag}",
                "ESP-IDF target: esp32s3",
                "",
                "网页刷机请使用 GitHub Pages WebFlash 页面。",
                "",
                "手动刷写示例:",
                "  esptool.py --chip esp32s3 write_flash $(cat flash_args)",
                "",
            ]
        ),
        encoding="utf-8",
    )

    webflash_bin = release_asset_dir / WEBFLASH_BIN
    if args.merge_mode == "raw":
        raw_merge(parts, webflash_bin)
    else:
        esptool = args.esptool or os.environ.get("ESPTOOL") or "esptool.py"
        try:
            merge_with_esptool(esptool, settings, parts, webflash_bin)
        except (FileNotFoundError, subprocess.CalledProcessError):
            require(args.merge_mode == "auto", "esptool merge_bin failed")
            print("WARN webflash package: esptool unavailable, using raw merge", file=sys.stderr)
            raw_merge(parts, webflash_bin)

    manifest = build_manifest(tag, args.repo, args.asset_base_url)
    manifest_path = release_asset_dir / WEBFLASH_MANIFEST
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    shutil.copy2(build_dir / "flasher_args.json", release_asset_dir / "flasher_args.json")
    project_description = build_dir / "project_description.json"
    if project_description.exists():
        shutil.copy2(project_description, release_asset_dir / "project_description.json")

    zip_path = dist_dir / package_name
    write_zip(package_dir, zip_path)
    shutil.copy2(zip_path, release_asset_dir / package_name)

    checksum_inputs = [path for path in release_asset_dir.iterdir() if path.is_file() and path.name != CHECKSUMS]
    write_checksums(checksum_inputs, release_asset_dir / CHECKSUMS)

    asset_names = sorted(path.name for path in release_asset_dir.iterdir() if path.is_file())
    return PackageResult(package_name=package_name, asset_names=asset_names, release_asset_dir=release_asset_dir)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Package release assets for WebFlash.")
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--dist-dir", type=Path, default=Path("dist"))
    parser.add_argument("--firmware-name", default="lofi_cardputer")
    parser.add_argument("--release-tag", required=True)
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY"))
    parser.add_argument("--commit", default=os.environ.get("GITHUB_SHA"))
    parser.add_argument("--asset-base-url")
    parser.add_argument("--esptool")
    parser.add_argument("--merge-mode", choices=["auto", "esptool", "raw"], default="auto")
    return parser.parse_args()


def main() -> None:
    result = package_webflash(parse_args())
    print(f"package-name={result.package_name}")
    print(f"release-asset-dir={result.release_asset_dir}")
    print("asset-names=" + ",".join(result.asset_names))


if __name__ == "__main__":
    main()
