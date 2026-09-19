#!/usr/bin/env python3
"""Deterministic generator for the Session 49 glTF fixtures (stdlib only).

Writes, next to this script:

  quad.gltf      — the textured 4x4 quad, byte-for-byte the same geometry
                   and UV mapping as quad.obj: positions (0,0.5,0),
                   (4,0.5,0), (4,0.5,4), (0,0.5,4); two indexed triangles
                   0-1-2 / 0-2-3; texcoords stored in glTF's TOP-left
                   origin ((0,1),(1,1),(1,0),(0,0)) so the loader's V flip
                   reproduces quad.obj's bottom-left vt values exactly.
                   Buffer embedded as a base64 data URI (single-file
                   fixture); texture referenced externally as checker.png.
  quad.glb       — the same asset in GLB binary form with checker.png
                   EMBEDDED in the BIN chunk (exercises the
                   map_kd_bytes / load_from_memory path).
  emissive.glb   — Session 51: the same quad + embedded checker.png base
                   colour, PLUS emissiveFactor [1,1,1] and an embedded 2x1
                   emissiveTexture (left texel white, right black) — the
                   Sketchfab-standard emissive-mask pattern. With u = x/4,
                   voxels vx<2 sample the white texel (per-voxel ke -> 1,
                   emissive branch) and vx>=2 the black texel (ke -> 0,
                   opaque branch with the checker colour).
  materials.gltf — three untextured single-triangle primitives sharing
                   one POSITION accessor (non-indexed path), one material
                   each, covering the PBR->Material mapping: opaque_red
                   (baseColorFactor), blend_half (alphaMode BLEND ->
                   d = baseColor.a), emissive_green (emissiveFactor x
                   KHR_materials_emissive_strength, clamped).

Regenerate with: python3 gen_gltf_fixtures.py   (output is deterministic:
sorted JSON keys, no timestamps).
"""

import base64
import json
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))

GREY = 0.21404114  # linear value whose sRGB encoding is ~0.5 (quad.mtl Kd)


def quad_geometry():
    positions = [(0.0, 0.5, 0.0), (4.0, 0.5, 0.0),
                 (4.0, 0.5, 4.0), (0.0, 0.5, 4.0)]
    uvs = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]  # top-left origin
    indices = [0, 1, 2, 0, 2, 3]
    pos_b = b"".join(struct.pack("<fff", *p) for p in positions)  # 48 B @ 0
    uv_b = b"".join(struct.pack("<ff", *t) for t in uvs)          # 32 B @ 48
    idx_b = struct.pack("<6H", *indices)                          # 12 B @ 80
    return pos_b + uv_b + idx_b  # 92 bytes


def quad_json(buffer_entry, images_entry, extra_buffer_views):
    return {
        "asset": {"version": "2.0", "generator": "objtomc fixture gen"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{
            "primitives": [{
                "attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                "indices": 2,
                "material": 0,
                "mode": 4,
            }],
        }],
        "materials": [{
            "name": "quadmat",
            "pbrMetallicRoughness": {
                "baseColorFactor": [GREY, GREY, GREY, 1.0],
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0.0,
                "roughnessFactor": 1.0,
            },
        }],
        "textures": [{"source": 0}],
        "images": images_entry,
        "buffers": [buffer_entry],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 48},
            {"buffer": 0, "byteOffset": 48, "byteLength": 32},
            {"buffer": 0, "byteOffset": 80, "byteLength": 12},
        ] + extra_buffer_views,
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4,
             "type": "VEC3",
             "min": [0.0, 0.5, 0.0], "max": [4.0, 0.5, 4.0]},
            {"bufferView": 1, "componentType": 5126, "count": 4,
             "type": "VEC2"},
            {"bufferView": 2, "componentType": 5123, "count": 6,
             "type": "SCALAR"},
        ],
    }


def dump_json(doc):
    return json.dumps(doc, sort_keys=True, separators=(",", ":"))


def write_quad_gltf():
    geo = quad_geometry()
    doc = quad_json(
        buffer_entry={
            "byteLength": len(geo),
            "uri": "data:application/octet-stream;base64,"
                   + base64.b64encode(geo).decode("ascii"),
        },
        images_entry=[{"uri": "checker.png"}],
        extra_buffer_views=[],
    )
    with open(os.path.join(HERE, "quad.gltf"), "w") as fh:
        fh.write(dump_json(doc) + "\n")


def write_quad_glb():
    geo = quad_geometry()
    with open(os.path.join(HERE, "checker.png"), "rb") as fh:
        png = fh.read()
    # geo is 92 bytes (4-aligned); png bufferView follows at offset 92.
    bin_chunk = geo + png
    doc = quad_json(
        buffer_entry={"byteLength": len(bin_chunk)},
        images_entry=[{"bufferView": 3, "mimeType": "image/png",
                       "name": "checker"}],
        extra_buffer_views=[
            {"buffer": 0, "byteOffset": len(geo), "byteLength": len(png)},
        ],
    )
    json_b = dump_json(doc).encode("utf-8")
    json_b += b" " * ((4 - len(json_b) % 4) % 4)      # pad with spaces
    bin_b = bin_chunk + b"\x00" * ((4 - len(bin_chunk) % 4) % 4)
    total = 12 + 8 + len(json_b) + 8 + len(bin_b)
    with open(os.path.join(HERE, "quad.glb"), "wb") as fh:
        fh.write(struct.pack("<III", 0x46546C67, 2, total))       # glTF v2
        fh.write(struct.pack("<II", len(json_b), 0x4E4F534A))     # JSON
        fh.write(json_b)
        fh.write(struct.pack("<II", len(bin_b), 0x004E4942))      # BIN
        fh.write(bin_b)


def make_png_rgb8(width, height, pixels):
    """Minimal deterministic PNG encoder (truecolor 8-bit, no interlace).
    pixels: list of (r, g, b) rows-major, top row first."""
    import zlib

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload)))

    raw = b""
    for y in range(height):
        raw += b"\x00"  # filter type 0 per scanline
        for x in range(width):
            raw += bytes(pixels[y * width + x])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def write_emissive_glb():
    geo = quad_geometry()
    with open(os.path.join(HERE, "checker.png"), "rb") as fh:
        checker = fh.read()
    mask = make_png_rgb8(2, 1, [(255, 255, 255), (0, 0, 0)])
    # BIN layout: geo (92 B, 4-aligned) | checker | pad | mask.
    checker_off = len(geo)
    mask_off = checker_off + len(checker)
    pad = (4 - mask_off % 4) % 4
    mask_off += pad
    bin_chunk = geo + checker + b"\x00" * pad + mask
    doc = quad_json(
        buffer_entry={"byteLength": len(bin_chunk)},
        images_entry=[
            {"bufferView": 3, "mimeType": "image/png", "name": "checker"},
            {"bufferView": 4, "mimeType": "image/png",
             "name": "emissive_mask"},
        ],
        extra_buffer_views=[
            {"buffer": 0, "byteOffset": checker_off,
             "byteLength": len(checker)},
            {"buffer": 0, "byteOffset": mask_off, "byteLength": len(mask)},
        ],
    )
    doc["textures"] = [{"source": 0}, {"source": 1}]
    doc["materials"][0]["emissiveFactor"] = [1.0, 1.0, 1.0]
    doc["materials"][0]["emissiveTexture"] = {"index": 1}
    json_b = dump_json(doc).encode("utf-8")
    json_b += b" " * ((4 - len(json_b) % 4) % 4)      # pad with spaces
    bin_b = bin_chunk + b"\x00" * ((4 - len(bin_chunk) % 4) % 4)
    total = 12 + 8 + len(json_b) + 8 + len(bin_b)
    with open(os.path.join(HERE, "emissive.glb"), "wb") as fh:
        fh.write(struct.pack("<III", 0x46546C67, 2, total))       # glTF v2
        fh.write(struct.pack("<II", len(json_b), 0x4E4F534A))     # JSON
        fh.write(json_b)
        fh.write(struct.pack("<II", len(bin_b), 0x004E4942))      # BIN
        fh.write(bin_b)


def write_materials_gltf():
    positions = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)]
    pos_b = b"".join(struct.pack("<fff", *p) for p in positions)
    prim = {"attributes": {"POSITION": 0}, "mode": 4}
    doc = {
        "asset": {"version": "2.0", "generator": "objtomc fixture gen"},
        "extensionsUsed": ["KHR_materials_emissive_strength"],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{
            # Three non-indexed primitives sharing the POSITION accessor,
            # one per material under test.
            "primitives": [
                dict(prim, material=0),
                dict(prim, material=1),
                dict(prim, material=2),
            ],
        }],
        "materials": [
            {"name": "opaque_red",
             "pbrMetallicRoughness": {
                 "baseColorFactor": [1.0, 0.0, 0.0, 1.0]}},
            {"name": "blend_half",
             "alphaMode": "BLEND",
             "pbrMetallicRoughness": {
                 "baseColorFactor": [0.0, 0.0, 1.0, 0.4]}},
            {"name": "emissive_green",
             "emissiveFactor": [0.0, 0.5, 0.0],
             "extensions": {
                 "KHR_materials_emissive_strength": {
                     "emissiveStrength": 2.0}}},
        ],
        "buffers": [{
            "byteLength": len(pos_b),
            "uri": "data:application/octet-stream;base64,"
                   + base64.b64encode(pos_b).decode("ascii"),
        }],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_b)},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3,
             "type": "VEC3",
             "min": [0.0, 0.0, 0.0], "max": [1.0, 1.0, 0.0]},
        ],
    }
    with open(os.path.join(HERE, "materials.gltf"), "w") as fh:
        fh.write(dump_json(doc) + "\n")


if __name__ == "__main__":
    write_quad_gltf()
    write_quad_glb()
    write_emissive_glb()
    write_materials_gltf()
    print("wrote quad.gltf, quad.glb, emissive.glb, materials.gltf")
