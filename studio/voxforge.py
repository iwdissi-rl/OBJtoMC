#!/usr/bin/env python3.12
"""VOXFORGE — one-click 3D-model → Minecraft-world converter GUI.

Wraps the objtomc pipeline (build-release/objtomc) behind a local web UI:
drop a model (or paste a path), pick a size, hit FORGE, watch the live
console, then INSTALL the world straight into ~/.minecraft/saves.

Run:  python3.12 studio/voxforge.py   →  http://localhost:7777
"""

import json
import queue
import re
import shutil
import subprocess
import threading
import time
import zipfile
from pathlib import Path

from flask import Flask, Response, jsonify, request, send_from_directory

ROOT       = Path(__file__).resolve().parent
OBJTOMC    = ROOT.parent / "build-release" / "objtomc"
UPLOADS    = ROOT / "uploads"
WORLDS     = ROOT / "worlds"
SAVES      = Path.home() / ".minecraft" / "saves"
UPLOADS.mkdir(exist_ok=True)
WORLDS.mkdir(exist_ok=True)

app = Flask(__name__, static_folder=str(ROOT / "static"), static_url_path="")

jobs = {}          # id -> dict(status, lines, world, name, stats)
jobs_lock = threading.Lock()
job_seq = [0]


def find_model(path: Path):
    """Locate the model file under `path` (which may itself be the file)."""
    if path.is_file():
        if path.suffix.lower() in (".glb", ".gltf", ".obj"):
            return path
        if path.suffix.lower() == ".zip":
            dest = UPLOADS / f"x{int(time.time())}_{path.stem}"
            dest.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(path) as z:
                z.extractall(dest)
            path = dest
        else:
            return None
    for ext in (".glb", ".gltf", ".obj"):
        hits = sorted(path.rglob(f"*{ext}"))
        if hits:
            return hits[0]
    return None


def run_job(jid, model: Path, name: str, size: int, cleanup: bool,
            align: bool):
    j = jobs[jid]
    out = WORLDS / name
    if out.exists():
        shutil.rmtree(out)
    cmd = [str(OBJTOMC), "--target-size", str(size), "--tiling", "auto",
           "--world-origin", "0,-64,0", "--level-name", name,
           "--verbose"]
    if cleanup:
        cmd += ["--cleanup", "on"]
    if align:
        cmd += ["--align", "auto"]
    cmd += [str(model), str(out)]
    j["lines"].put(f"$ {' '.join(cmd)}")
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                bufsize=1)
        for line in proc.stdout:
            line = line.rstrip("\n")
            j["lines"].put(line)
            m = re.search(r"occupied = (\d+)", line)
            if m:
                j["stats"]["voxels"] = int(m.group(1))
            m = re.search(r"grid dims = (.+)", line)
            if m:
                j["stats"]["dims"] = m.group(1)
            if "extended world height" in line:
                j["stats"]["extended"] = True
        proc.wait()
        if proc.returncode == 0 and (out / "level.dat").exists():
            j["world"] = str(out)
            regions = len(list((out / "region").glob("*.mca")))
            j["stats"]["regions"] = regions
            j["status"] = "done"
            j["lines"].put(f":: FORGE COMPLETE — {regions} region file(s)")
        else:
            j["status"] = "failed"
            j["lines"].put(f":: FAILED (exit {proc.returncode})")
    except Exception as e:                                  # noqa: BLE001
        j["status"] = "failed"
        j["lines"].put(f":: ERROR {e}")
    j["lines"].put(None)                                    # sentinel: stream end


@app.get("/")
def index():
    return send_from_directory(app.static_folder, "index.html")


@app.post("/api/convert")
def convert():
    name    = re.sub(r"[^A-Za-z0-9_ -]", "", request.form.get("name", "")).strip()
    size    = max(16, min(16384, int(request.form.get("size", "1024"))))
    cleanup = request.form.get("cleanup", "1") == "1"
    align   = request.form.get("align", "0") == "1"

    up = request.files.get("file")
    if up and up.filename:
        dest = UPLOADS / f"u{int(time.time())}_{Path(up.filename).name}"
        up.save(dest)
        src = dest
    else:
        p = request.form.get("path", "").strip()
        if not p:
            return jsonify(error="no file or path given"), 400
        src = Path(p).expanduser()
        if not src.exists():
            return jsonify(error=f"path not found: {src}"), 400

    model = find_model(src)
    if model is None:
        return jsonify(error="no .glb/.gltf/.obj found in the input"), 400
    if not name:
        name = model.stem if model.stem != "scene" else model.parent.name

    with jobs_lock:
        job_seq[0] += 1
        jid = str(job_seq[0])
        jobs[jid] = {"status": "running", "lines": queue.Queue(),
                     "world": None, "name": name, "size": size,
                     "stats": {}, "started": time.strftime("%H:%M:%S")}
    threading.Thread(target=run_job,
                     args=(jid, model, name, size, cleanup, align),
                     daemon=True).start()
    return jsonify(job=jid, name=name, model=str(model))


@app.get("/api/log/<jid>")
def log(jid):
    j = jobs.get(jid)
    if not j:
        return jsonify(error="no such job"), 404

    def stream():
        while True:
            line = j["lines"].get()
            if line is None:
                yield f"event: end\ndata: {j['status']}\n\n"
                return
            yield f"data: {json.dumps(line)}\n\n"
    return Response(stream(), mimetype="text/event-stream")


@app.get("/api/jobs")
def list_jobs():
    out = []
    for jid, j in sorted(jobs.items(), key=lambda kv: int(kv[0]),
                         reverse=True):
        out.append({"job": jid, "name": j["name"], "status": j["status"],
                    "size": j["size"], "stats": j["stats"],
                    "started": j["started"],
                    "installed": (SAVES / j["name"]).exists()})
    return jsonify(out)


@app.post("/api/install/<jid>")
def install(jid):
    j = jobs.get(jid)
    if not j or j["status"] != "done":
        return jsonify(error="job not done"), 400
    dest = SAVES / j["name"]
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(j["world"], dest)
    return jsonify(ok=True, installed=str(dest))


if __name__ == "__main__":
    print(f"VOXFORGE :: objtomc = {OBJTOMC}")
    app.run(host="127.0.0.1", port=7777, threaded=True)
