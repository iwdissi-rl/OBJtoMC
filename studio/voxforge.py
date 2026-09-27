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

jobs = {}          # id -> dict(status, lines, world, name, stats, model)
ORACLE_PY  = ROOT.parent / "tests" / "oracle_venv" / "bin" / "python"
PALETTE_CSV = ROOT.parent / "data" / "block_palette_db.csv"
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


def _stream(j, cmd):
    """Run `cmd`, stream stdout into the job feed. Returns exit code."""
    j["lines"].put(f"$ {' '.join(cmd)}")
    j["log_all"].append(f"$ {' '.join(cmd)}")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)
    for line in proc.stdout:
        line = line.rstrip("\n")
        j["lines"].put(line)
        j["log_all"].append(line)
        m = re.search(r"occupied = (\d+)", line)
        if m:
            j["stats"]["voxels"] = int(m.group(1))
        m = re.search(r"grid dims = (.+)", line)
        if m:
            j["stats"]["dims"] = m.group(1)
        if "extended" in line and "height" in line:
            j["stats"]["extended"] = True
    proc.wait()
    return proc.returncode


def _fail(j, msg):
    j["status"] = "failed"
    j["lines"].put(msg)
    j["log_all"].append(msg)
    j["lines"].put(None)


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
    try:
        rc = _stream(j, cmd)
        if rc == 0 and (out / "level.dat").exists():
            j["world"] = str(out)
            regions = len(list((out / "region").glob("*.mca")))
            j["stats"]["regions"] = regions
            j["status"] = "done"
            j["lines"].put(f":: FORGE COMPLETE — {regions} region file(s)")
            j["log_all"].append(f":: FORGE COMPLETE — {regions} region file(s)")
        else:
            return _fail(j, f":: FAILED (exit {rc})")
    except Exception as e:                                  # noqa: BLE001
        return _fail(j, f":: ERROR {e}")
    j["lines"].put(None)                                    # sentinel: stream end


def run_import_job(jid, model: Path, name: str, size: int, cleanup: bool,
                   align: bool, dst: Path, x: int, y: str, z: int,
                   sink: int):
    """Convert the model to a temp world, then merge it into `dst`."""
    j = jobs[jid]
    out = WORLDS / f"__import_{name}"
    if out.exists():
        shutil.rmtree(out)
    cmd = [str(OBJTOMC), "--target-size", str(size), "--tiling", "auto",
           "--world-origin", "0,-64,0", "--level-name", name, "--verbose"]
    if cleanup:
        cmd += ["--cleanup", "on"]
    if align:
        cmd += ["--align", "auto"]
    cmd += [str(model), str(out)]
    try:
        rc = _stream(j, cmd)
        if rc != 0 or not (out / "level.dat").exists():
            return _fail(j, f":: FAILED (convert exit {rc})")
        j["lines"].put(f":: convert done — merging into '{dst.name}' "
                       f"at ({x},{y},{z}) ...")
        j["log_all"].append(":: convert done — merging ...")
        rc = _stream(j, [str(ORACLE_PY), str(ROOT / "world_merge.py"),
                         str(out), str(dst), "--at", f"{x},{y},{z}",
                         "--sink", str(sink)])
        done = any("IMPORT COMPLETE" in ln for ln in j["log_all"][-5:])
        if rc != 0 or not done:
            return _fail(j, f":: FAILED (merge exit {rc}) — world backup "
                            f"under {dst.name}/voxforge_backup_*")
        j["status"] = "done"
    except Exception as e:                                  # noqa: BLE001
        return _fail(j, f":: ERROR {e}")
    finally:
        if out.exists():
            shutil.rmtree(out, ignore_errors=True)
    j["lines"].put(None)


@app.get("/")
def index():
    return send_from_directory(app.static_folder, "index.html")


@app.get("/api/saves")
def saves():
    """Existing worlds in ~/.minecraft/saves that can be import targets."""
    out = []
    if SAVES.exists():
        for d in sorted(SAVES.iterdir()):
            if (d / "level.dat").exists() and (d / "region").exists():
                mb = sum(f.stat().st_size
                         for f in (d / "region").glob("*.mca")) // 2**20
                out.append({"name": d.name, "mb": mb})
    return jsonify(out)


def _job_input(defaults_name=True):
    """Shared upload/path/model resolution for convert + import."""
    up = request.files.get("file")
    if up and up.filename:
        dest = UPLOADS / f"u{int(time.time())}_{Path(up.filename).name}"
        up.save(dest)
        src = dest
    else:
        p = request.form.get("path", "").strip()
        if not p:
            return None, (jsonify(error="no file or path given"), 400)
        src = Path(p).expanduser()
        if not src.exists():
            return None, (jsonify(error=f"path not found: {src}"), 400)
    model = find_model(src)
    if model is None:
        return None, (jsonify(error="no .glb/.gltf/.obj found in the input"),
                      400)
    return model, None


@app.post("/api/import")
def do_import():
    """Convert a model and merge it into an existing save."""
    name    = re.sub(r"[^A-Za-z0-9_ -]", "",
                     request.form.get("name", "")).strip()
    size    = max(16, min(16384, int(request.form.get("size", "256"))))
    cleanup = request.form.get("cleanup", "1") == "1"
    align   = request.form.get("align", "0") == "1"
    sink    = max(0, min(256, int(request.form.get("sink", "0") or 0)))
    dstname = request.form.get("dst", "").strip()
    dst = SAVES / dstname
    if not dstname or not (dst / "level.dat").exists():
        return jsonify(error=f"target world not found: {dstname}"), 400
    try:
        x = int(request.form.get("x", "0") or 0)
        z = int(request.form.get("z", "0") or 0)
    except ValueError:
        return jsonify(error="X/Z must be integers"), 400
    y = request.form.get("y", "auto").strip() or "auto"
    if y != "auto":
        try:
            int(y)
        except ValueError:
            return jsonify(error="Y must be an integer or 'auto'"), 400

    model, err = _job_input()
    if err:
        return err
    if not name:
        name = model.stem if model.stem != "scene" else model.parent.name

    with jobs_lock:
        job_seq[0] += 1
        jid = str(job_seq[0])
        jobs[jid] = {"status": "running", "lines": queue.Queue(),
                     "world": None, "name": name, "size": size,
                     "stats": {}, "started": time.strftime("%H:%M:%S"),
                     "model": str(model), "cache": {}, "log_all": [],
                     "mode": "import", "dst": dstname,
                     "at": f"{x},{y},{z}"}
    threading.Thread(target=run_import_job,
                     args=(jid, model, name, size, cleanup, align, dst,
                           x, y, z, sink),
                     daemon=True).start()
    return jsonify(job=jid, name=name, model=str(model))


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
                     "stats": {}, "started": time.strftime("%H:%M:%S"),
                     "model": str(model), "cache": {}, "log_all": []}
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
                    "mode": j.get("mode", "forge"),
                    "dst": j.get("dst"), "at": j.get("at"),
                    "installed": (SAVES / j["name"]).exists()})
    return jsonify(out)


@app.post("/api/debugshot")
def debugshot():
    import base64
    t = re.sub(r"[^0-9a-z]", "", request.args.get("t", "0"))
    data = request.get_data(as_text=True)
    if data.startswith("data:image/png;base64,"):
        (ROOT / f"debug_shot_{t}.png").write_bytes(
            base64.b64decode(data.split(",", 1)[1]))
    return jsonify(ok=True)


@app.post("/api/debuglog")
def debuglog():
    with open(ROOT / "debug_js.log", "a") as f:
        f.write(request.get_data(as_text=True) + "\n")
    return jsonify(ok=True)


@app.get("/api/loglines/<jid>")
def loglines(jid):
    j = jobs.get(jid)
    if not j:
        return jsonify(error="no such job"), 404
    return jsonify(lines=j["log_all"], status=j["status"],
                   name=j["name"], size=j["size"], stats=j["stats"])


@app.get("/api/palette")
def palette():
    out = []
    for line in PALETTE_CSV.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        f = line.split(",")
        out.append([f[0].removeprefix("minecraft:"),
                    int(f[3]), int(f[4]), int(f[5]),
                    int(f[1]), int(f[2]), f[8] == "true"])
    return jsonify(out)


def _cached_sampler(jid, key, cmd):
    j = jobs.get(jid)
    if not j:
        return jsonify(error="no such job"), 404
    if key not in j["cache"]:
        try:
            r = subprocess.run(cmd, capture_output=True, text=True,
                               timeout=600)
            if r.returncode != 0:
                return jsonify(error=r.stderr[-400:]), 500
            j["cache"][key] = r.stdout
        except Exception as e:                              # noqa: BLE001
            return jsonify(error=str(e)), 500
    return Response(j["cache"][key], mimetype="application/json")


@app.get("/api/mesh/<jid>")
def mesh_points(jid):
    j = jobs.get(jid)
    if not j:
        return jsonify(error="no such job"), 404
    return _cached_sampler(jid, "mesh",
                           ["python3.12", str(ROOT / "mesh_sampler.py"),
                            j["model"]])


@app.get("/api/voxels/<jid>")
def voxels(jid):
    j = jobs.get(jid)
    if not j:
        return jsonify(error="no such job"), 404
    if j["status"] != "done":
        return jsonify(error="not done"), 400
    return _cached_sampler(jid, "voxels",
                           [str(ORACLE_PY), str(ROOT / "world_sampler.py"),
                            j["world"], str(PALETTE_CSV)])


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
