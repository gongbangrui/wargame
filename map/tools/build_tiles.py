#!/usr/bin/env python3
"""Build the Qt simulation's offline XYZ tile pyramid from the canonical master image."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil

from PIL import Image, ImageEnhance, ImageFilter, ImageOps


TILE_SIZE = 256
MIN_ZOOM = 12
MAX_ZOOM = 14
BASE_X_MIN, BASE_X_MAX = 3404, 3409
BASE_Y_MIN, BASE_Y_MAX = 1745, 1750
PROJECT_ORIGIN_LAT = 25.40
PROJECT_ORIGIN_LON = 119.30
DEFAULT_WIDTH_METERS = 20_000
DEFAULT_HEIGHT_METERS = 15_000


def tile_to_lon(x: int, zoom: int) -> float:
    return x / (1 << zoom) * 360.0 - 180.0


def tile_to_lat(y: int, zoom: int) -> float:
    n = math.pi - 2.0 * math.pi * y / (1 << zoom)
    return math.degrees(math.atan(math.sinh(n)))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tile_set_sha256(root: Path, tiles: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in sorted(tiles):
        digest.update(path.relative_to(root).as_posix().encode("ascii"))
        digest.update(b"\0")
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
    return digest.hexdigest()


def prepare_master(source: Path, target_side: int) -> Image.Image:
    with Image.open(source) as image:
        image.load()
        rgb = ImageOps.exif_transpose(image).convert("RGB")

    side = min(rgb.size)
    left = (rgb.width - side) // 2
    top = (rgb.height - side) // 2
    rgb = rgb.crop((left, top, left + side, top + side))

    rgb = rgb.resize((target_side, target_side), Image.Resampling.LANCZOS)
    rgb = ImageEnhance.Contrast(rgb).enhance(1.04)
    rgb = ImageEnhance.Color(rgb).enhance(0.94)
    return rgb.filter(ImageFilter.UnsharpMask(radius=0.8, percent=45, threshold=3))


def level_range(zoom: int) -> tuple[int, int, int, int]:
    factor = 1 << (zoom - MIN_ZOOM)
    return (
        BASE_X_MIN * factor,
        (BASE_X_MAX + 1) * factor - 1,
        BASE_Y_MIN * factor,
        (BASE_Y_MAX + 1) * factor - 1,
    )


def write_tile_set(master: Image.Image, root: Path, zoom: int) -> list[Path]:
    written: list[Path] = []
    x_min, x_max, y_min, y_max = level_range(zoom)
    for column, tile_x in enumerate(range(x_min, x_max + 1)):
        tile_dir = root / str(zoom) / str(tile_x)
        tile_dir.mkdir(parents=True, exist_ok=True)
        for row, tile_y in enumerate(range(y_min, y_max + 1)):
            box = (
                column * TILE_SIZE,
                row * TILE_SIZE,
                (column + 1) * TILE_SIZE,
                (row + 1) * TILE_SIZE,
            )
            tile_path = tile_dir / f"{tile_y}.png"
            master.crop(box).save(tile_path, format="PNG", optimize=True, compress_level=9)
            written.append(tile_path)
    return written


def write_metadata(root: Path, source: Path, tiles: list[Path]) -> None:
    base_x_min, base_x_max, base_y_min, base_y_max = level_range(MIN_ZOOM)
    west = tile_to_lon(base_x_min, MIN_ZOOM)
    east = tile_to_lon(base_x_max + 1, MIN_ZOOM)
    north = tile_to_lat(base_y_min, MIN_ZOOM)
    south = tile_to_lat(base_y_max + 1, MIN_ZOOM)
    center_lon = (west + east) / 2.0
    center_lat = (south + north) / 2.0
    tile_ranges = {}
    for zoom in range(MIN_ZOOM, MAX_ZOOM + 1):
        x_min, x_max, y_min, y_max = level_range(zoom)
        tile_ranges[str(zoom)] = {
            "xMin": x_min,
            "xMax": x_max,
            "yMin": y_min,
            "yMax": y_max,
            "count": (x_max - x_min + 1) * (y_max - y_min + 1),
        }

    metadata = {
        "schemaVersion": 1,
        "revision": 1,
        "mapId": "coastal-mountain-theater-v1",
        "name": "海岸山地联合作战区",
        "description": "为兵棋推演 Qt 项目原创生成的虚构卫星地图",
        "source": {
            "type": "AI-generated original fictional raster",
            "master": str(source.relative_to(root)),
            "prompt": "prompt.txt",
            "sha256": sha256(source),
        },
        "projection": "EPSG:3857",
        "scheme": "xyz",
        "tileSize": TILE_SIZE,
        "format": "png",
        "minZoom": MIN_ZOOM,
        "maxZoom": MAX_ZOOM,
        "tileRange": {
            "xMin": base_x_min,
            "xMax": base_x_max,
            "yMin": base_y_min,
            "yMax": base_y_max,
            "count": tile_ranges[str(MIN_ZOOM)]["count"],
        },
        "tileRanges": tile_ranges,
        "tileSetSha256": tile_set_sha256(root, tiles),
        "boundsWgs84": [west, south, east, north],
        "projectAlignment": {
            "originLat": PROJECT_ORIGIN_LAT,
            "originLon": PROJECT_ORIGIN_LON,
            "logicalWidthMeters": DEFAULT_WIDTH_METERS,
            "logicalHeightMeters": DEFAULT_HEIGHT_METERS,
            "logicalXAxis": "east",
            "logicalYAxis": "north",
        },
        "content": [
            "山脉与岩壁",
            "森林与灌木",
            "河流与支流",
            "水库与大坝",
            "湿地与滩涂",
            "沙滩与岩岸",
            "岛礁",
            "梯田与农田",
            "村镇与工业区",
            "公路、铁路、桥梁与隧道",
            "港口、采石场与小型机场",
        ],
    }
    (root / "metadata.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    tilejson = {
        "tilejson": "3.0.0",
        "version": "1.0.0",
        "name": metadata["name"],
        "description": metadata["description"],
        "scheme": "xyz",
        "format": "png",
        "minzoom": MIN_ZOOM,
        "maxzoom": MAX_ZOOM,
        "bounds": [west, south, east, north],
        "center": [center_lon, center_lat, MIN_ZOOM],
        "tiles": ["./{z}/{x}/{y}.png"],
        "attribution": "原创虚构卫星地图",
    }
    (root / "tilejson.json").write_text(
        json.dumps(tilejson, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def write_preview(master: Image.Image, root: Path) -> None:
    preview = master.resize((1024, 1024), Image.Resampling.LANCZOS)
    preview.save(root / "preview.png", format="PNG", optimize=True, compress_level=9)


def write_checksums(root: Path, source: Path, files: list[Path]) -> None:
    entries = sorted(
        files
        + [
            source,
            root / "prompt.txt",
            root / "metadata.json",
            root / "tilejson.json",
            root / "preview.png",
        ]
    )
    lines = [f"{sha256(path)}  {path.relative_to(root)}" for path in entries]
    (root / "MANIFEST.sha256").write_text("\n".join(lines) + "\n", encoding="ascii")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()

    root = args.root.resolve()
    source = args.source.resolve()
    if not source.is_file():
        raise SystemExit(f"Master image not found: {source}")
    try:
        source.relative_to(root)
    except ValueError as exc:
        raise SystemExit("Master image must be stored inside the map directory") from exc

    for zoom in range(MIN_ZOOM, MAX_ZOOM + 1):
        shutil.rmtree(root / str(zoom), ignore_errors=True)

    base_side = (BASE_X_MAX - BASE_X_MIN + 1) * TILE_SIZE
    max_x_min, max_x_max, _, _ = level_range(MAX_ZOOM)
    max_side = (max_x_max - max_x_min + 1) * TILE_SIZE
    canonical_master = prepare_master(source, base_side)
    # Keep z12 as the geographic authority. Higher levels are deterministic
    # derivatives until an alignment-checked high-resolution master is supplied.
    detail_master = canonical_master.resize((max_side, max_side), Image.Resampling.LANCZOS)
    detail_master = detail_master.filter(
        ImageFilter.UnsharpMask(radius=1.1, percent=55, threshold=3)
    )

    tiles: list[Path] = []
    for zoom in range(MIN_ZOOM, MAX_ZOOM + 1):
        if zoom == MIN_ZOOM:
            level_master = canonical_master
        else:
            x_min, x_max, _, _ = level_range(zoom)
            side = (x_max - x_min + 1) * TILE_SIZE
            level_master = detail_master.resize((side, side), Image.Resampling.LANCZOS)
        tiles.extend(write_tile_set(level_master, root, zoom))

    write_preview(canonical_master, root)
    write_metadata(root, source, tiles)
    write_checksums(root, source, tiles)
    print(f"Built {len(tiles)} tiles at z{MIN_ZOOM}..{MAX_ZOOM}")


if __name__ == "__main__":
    main()
