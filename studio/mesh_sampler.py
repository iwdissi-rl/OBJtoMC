#!/usr/bin/env python3.12
"""mesh_sampler — extract a point-cloud sample from a .obj/.gltf/.glb mesh.

Pure stdlib. Prints JSON: {"points": [x0,y0,z0, x1,y1,z1, ...] (normalised
to fit [-1,1] with Y up), "count_positions": N, "aabb": [dx,dy,dz]}.
Subsampled to <= CAP points.
"""
import base64
import json
import struct
import sys
from pathlib import Path

CAP = 60_000


def read_obj(path):
    pts = []
    with open(path, "rb") as f:
        for line in f:
            if line.startswith(b"v "):
                parts = line.split()
                pts.append((float(parts[1]), float(parts[2]),
                            float(parts[3])))
    return pts


def read_gltf(path):
    path = Path(path)
    if path.suffix.lower() == ".glb":
        raw = path.read_bytes()
        assert raw[:4] == b"glTF"
        off = 12
        doc = None
        bin_chunk = b""
        while off < len(raw):
            ln, typ = struct.unpack_from("<II", raw, off)
            body = raw[off + 8:off + 8 + ln]
            if typ == 0x4E4F534A:
                doc = json.loads(body)
            elif typ == 0x004E4942:
                bin_chunk = body
            off += 8 + ln
        buffers = [bin_chunk]
        base = path.parent
    else:
        doc = json.loads(path.read_text())
        base = path.parent
        buffers = []
        for b in doc.get("buffers", []):
            uri = b.get("uri", "")
            if uri.startswith("data:"):
                buffers.append(base64.b64decode(uri.split(",", 1)[1]))
            else:
                buffers.append((base / uri).read_bytes())

    views = doc.get("bufferViews", [])
    pts = []
    for mesh in doc.get("meshes", []):
        for prim in mesh.get("primitives", []):
            ai = prim.get("attributes", {}).get("POSITION")
            if ai is None:
                continue
            acc = doc["accessors"][ai]
            if acc.get("componentType") != 5126 or acc.get("type") != "VEC3":
                continue
            view = views[acc["bufferView"]]
            buf = buffers[view.get("buffer", 0)]
            start = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
            stride = view.get("byteStride") or 12
            n = acc["count"]
            for i in range(n):
                o = start + i * stride
                pts.append(struct.unpack_from("<fff", buf, o))
    return pts


def main():
    path = Path(sys.argv[1])
    pts = (read_obj(path) if path.suffix.lower() == ".obj"
           else read_gltf(path))
    n = len(pts)
    stride = max(1, (n + CAP - 1) // CAP)
    pts = pts[::stride]

    lo = [min(p[i] for p in pts) for i in range(3)]
    hi = [max(p[i] for p in pts) for i in range(3)]
    ext = [max(hi[i] - lo[i], 1e-9) for i in range(3)]
    s = 2.0 / max(ext)
    c = [(hi[i] + lo[i]) / 2 for i in range(3)]
    flat = []
    for p in pts:
        flat += [round((p[0] - c[0]) * s, 4), round((p[1] - c[1]) * s, 4),
                 round((p[2] - c[2]) * s, 4)]
    json.dump({"points": flat, "count_positions": n,
               "aabb": [round(e, 3) for e in ext]},
              sys.stdout, separators=(",", ":"))


if __name__ == "__main__":
    main()
