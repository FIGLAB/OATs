from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import Response, FileResponse, RedirectResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles
from pathlib import Path
import asyncio
import ipaddress
import json
import re
import shutil
import threading
import time
import urllib.request
from datetime import datetime
from zoneinfo import ZoneInfo

from oat_gemini import *

app = FastAPI()
app.mount("/static", StaticFiles(directory=Path(__file__).parent / "static"), name="static")

USERS_DIR = Path("user")
GENERATING_HTML_PATH = Path(__file__).parent / "generating.html"

# Per-device emotion generation progress: {device: {current, total, message, done}}
# Written from background threads via callbacks; protected by _progress_lock.
g_generation_progress: dict[str, dict] = {}
_progress_lock = threading.Lock()
_updated_lock  = threading.Lock()


def _updated_path(user_dir: Path) -> Path:
    return user_dir / "updated.json"


def _mark_files_updated(user_dir: Path, flat_names: list):
    """Mark files as updated (needs download) in updated.json — thread-safe."""
    path = _updated_path(user_dir)
    with _updated_lock:
        data = json.loads(path.read_text()) if path.exists() else {}
        for name in flat_names:
            data[name] = True
        path.write_text(json.dumps(data))


def _mark_files_downloaded(user_dir: Path, flat_names: list):
    """Mark files as downloaded (up to date) in updated.json — thread-safe."""
    path = _updated_path(user_dir)
    with _updated_lock:
        data = json.loads(path.read_text()) if path.exists() else {}
        for name in flat_names:
            data[name] = False
        path.write_text(json.dumps(data))


# Folders under a device dir that the ESP32 must never download: server-side only.
# "png" is the source art (the device uses the "bin" copies); "logs" holds inference
# and behavior history that only the web pages read.
DEVICE_SKIP_DIRS     = ("png", "logs")
DEVICE_SKIP_PREFIXES = tuple(d + "-" for d in DEVICE_SKIP_DIRS)


# ── Per-device local time ─────────────────────────────────────────────────────
# A device's timezone is looked up once from the public IP it connects from and
# cached as "timezone" in its config.json. Every timestamp for that device (the
# Gemini prompt, sensor_history.json, log filenames) uses that zone. Until a
# lookup succeeds the server falls back to default_timezone from config.py.
TZ_LOOKUP_RETRY_SEC = 3600
_tz_lookup_last: dict[str, float] = {}   # device -> epoch seconds of last attempt
_tz_lookup_lock = threading.Lock()


def _client_ip(request: Request) -> str:
    """Real client IP. X-Forwarded-For is trusted only when the hop came from Caddy on localhost."""
    host = request.client.host if request.client else ""
    if host in ("127.0.0.1", "::1"):
        fwd = request.headers.get("x-forwarded-for", "")
        if fwd:
            return fwd.split(",")[0].strip()
    return host


def _lookup_timezone(ip: str) -> str | None:
    """Blocking: resolve a public IP to an IANA timezone via ip-api.com. None on failure."""
    try:
        addr = ipaddress.ip_address(ip)
    except ValueError:
        return None
    if addr.is_private or addr.is_loopback or addr.is_link_local or addr.is_reserved:
        return None
    try:
        url = f"http://ip-api.com/json/{ip}?fields=status,timezone"
        with urllib.request.urlopen(url, timeout=5) as r:
            info = json.loads(r.read().decode())
        tz = info.get("timezone") if info.get("status") == "success" else None
        if tz:
            ZoneInfo(tz)  # raises if the name is not a valid zone
            return tz
    except Exception as e:
        print(f"[timezone] lookup failed for {ip}: {e}")
    return None


def _device_config(user_dir: Path) -> dict:
    path = user_dir / "config.json"
    try:
        return json.loads(path.read_text()) if path.exists() else {}
    except Exception:
        return {}


def _device_tz(user_dir: Path) -> ZoneInfo:
    tz = _device_config(user_dir).get("timezone")
    try:
        return ZoneInfo(tz) if tz else ZoneInfo(default_timezone)
    except Exception:
        return ZoneInfo(default_timezone)


def _device_now(user_dir: Path) -> datetime:
    """Current local time where the device is."""
    return datetime.now(_device_tz(user_dir))


async def _ensure_timezone(user_dir: Path, request: Request):
    """Store the device's timezone in config.json if it is not known yet. Retries hourly at most."""
    if _device_config(user_dir).get("timezone"):
        return
    device = user_dir.name
    now = time.time()
    with _tz_lookup_lock:
        if now - _tz_lookup_last.get(device, 0) < TZ_LOOKUP_RETRY_SEC:
            return
        _tz_lookup_last[device] = now
    ip = _client_ip(request)
    tz = await asyncio.to_thread(_lookup_timezone, ip)
    if not tz:
        return
    config = _device_config(user_dir)
    config["timezone"] = tz
    (user_dir / "config.json").write_text(json.dumps(config))
    print(f"[timezone] {device}: {tz} (from {ip})")

SETUP_HTML_PATH     = Path(__file__).parent / "avatar.html"
SENSORS_HTML_PATH   = Path(__file__).parent / "sensors.html"
BEHAVIORS_HTML_PATH = Path(__file__).parent / "behaviors.html"
HISTORY_HTML_PATH   = Path(__file__).parent / "history.html"
EMOTION_HTML_PATH   = Path(__file__).parent / "emotion.html"
LOGS_HTML_PATH      = Path(__file__).parent / "logs.html"
MAIN_HTML_PATH      = Path(__file__).parent / "main.html"
DEFAULTS_PATH       = Path(__file__).parent / "defaults.json"


@app.get("/main")
async def main_page():
    return FileResponse(MAIN_HTML_PATH, media_type="text/html")


@app.get("/has_sensor_history")
async def has_sensor_history(device: str = ""):
    if not device:
        return {"exists": False}
    return {"exists": (USERS_DIR / device / "sensor_history.json").exists()}


@app.get("/avatar")
async def avatar_page():
    return FileResponse(SETUP_HTML_PATH, media_type="text/html")


@app.get("/sensors")
async def sensors():
    return FileResponse(SENSORS_HTML_PATH, media_type="text/html")


@app.get("/behaviors")
async def behaviors():
    return FileResponse(BEHAVIORS_HTML_PATH, media_type="text/html")


@app.get("/history")
async def history_page():
    return FileResponse(HISTORY_HTML_PATH, media_type="text/html")


@app.get("/emotion")
async def emotion_page():
    return FileResponse(EMOTION_HTML_PATH, media_type="text/html")


@app.get("/logs")
async def logs_page():
    return FileResponse(LOGS_HTML_PATH, media_type="text/html")


@app.get("/latest_emotion")
async def latest_emotion(device: str = ""):
    """Return the most recent log entry (emotion + reason + sensor history) for a device."""
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    logs_dir = USERS_DIR / device / "logs"
    if not logs_dir.exists():
        raise HTTPException(status_code=404, detail="No logs found")
    log_files = sorted((f for f in logs_dir.glob("*.json") if not f.name.startswith("behavior_")),
                       key=lambda f: f.stat().st_mtime)
    if not log_files:
        raise HTTPException(status_code=404, detail="No logs found")
    latest = log_files[-1]
    data = json.loads(latest.read_text())
    data["timestamp"] = latest.stem.replace("_", " ")
    data["timestamp_epoch"] = int(latest.stat().st_mtime)   # absolute, zone-independent
    return data


@app.get("/emotion_image")
async def emotion_image(device: str = "", emotion: str = "", frame: int = 0):
    """Serve an emotion bin frame for a device (1-bit packed bitmap)."""
    if not device or not emotion:
        raise HTTPException(status_code=400, detail="Missing device or emotion")
    bin_path = USERS_DIR / device / "bin" / emotion / f"{frame}.bin"
    if not bin_path.exists():
        raise HTTPException(status_code=404, detail="Image not found")
    return Response(content=bin_path.read_bytes(), media_type="application/octet-stream")


@app.post("/clear_history")
async def clear_history(request: Request):
    """Queue a history-clear command for the device.
    Creates an empty 'clear_history' sentinel file and marks it updated so the
    device downloads and acts on it at next boot."""
    try:
        payload = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")
    device = payload.get("device", "").strip()
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    user_dir = USERS_DIR / device
    if not user_dir.exists():
        raise HTTPException(status_code=404, detail="Device not found")
    (user_dir / "clear_history").write_text("")
    _mark_files_updated(user_dir, ["clear_history"])
    return {"ok": True}


@app.get("/device_config")
async def device_config(device: str = ""):
    """Return config.json for a device (name, description, etc.)."""
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    config_path = USERS_DIR / device / "config.json"
    if not config_path.exists():
        return {}
    return json.loads(config_path.read_text())


@app.get("/sensor_history_data")
async def sensor_history_data(device: str = ""):
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    hist_path = USERS_DIR / device / "sensor_history.json"
    if not hist_path.exists():
        raise HTTPException(status_code=404, detail="No sensor history for this device")
    data = json.loads(hist_path.read_text())
    # Enrich each sensor entry with unit / min / max / level from config for the visualizer
    for name, info in data.get("sensors", {}).items():
        meta = SENSOR_METADATA.get(name, {})
        for key in ("unit", "min", "max"):
            if key in meta:
                info[key] = meta[key]
    return data


@app.get("/inference_log_data")
async def inference_log_data(device: str = ""):
    """Return all inference log entries for a device, sorted by timestamp."""
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    logs_dir = USERS_DIR / device / "logs"
    if not logs_dir.exists():
        raise HTTPException(status_code=404, detail="No logs found")
    log_files = sorted((f for f in logs_dir.glob("*.json") if not f.name.startswith("behavior_")),
                       key=lambda f: f.stat().st_mtime)
    if not log_files:
        raise HTTPException(status_code=404, detail="No logs found")
    entries = []
    sensor_names = set()
    for f in log_files:
        try:
            data = json.loads(f.read_text())
        except Exception:
            continue
        timestamp = f.stem.replace("_", " ")
        sensors = data.get("sensor_history", {}).get("sensors", {})
        current_values = {}
        for name, info in sensors.items():
            current_values[name] = info.get("current value")
            sensor_names.add(name)
        entries.append({
            "timestamp": timestamp,
            "emotion": data.get("emotion", ""),
            "reason": data.get("reason", ""),
            "sensors": current_values,
        })
    # Collect behavior change logs
    behavior_changes = []
    for f in sorted(logs_dir.glob("behavior_*.json"), key=lambda f: f.stat().st_mtime):
        try:
            data = json.loads(f.read_text())
        except Exception:
            continue
        # filename: behavior_2026-03-03_17-47-01.json → timestamp
        ts = f.stem.replace("behavior_", "").replace("_", " ", 1)
        behavior_changes.append({
            "timestamp": ts,
            "positive": data.get("positive", ""),
            "negative": data.get("negative", ""),
        })

    # Include metadata for each sensor seen in logs
    metadata = {}
    for name in sensor_names:
        meta = SENSOR_METADATA.get(name, {})
        if meta:
            metadata[name] = {k: meta[k] for k in ("unit", "min", "max") if k in meta}
    return {"entries": entries, "metadata": metadata, "behavior_changes": behavior_changes}


@app.get("/generating")
async def generating_page():
    return FileResponse(GENERATING_HTML_PATH, media_type="text/html")


@app.get("/generation_progress")
async def generation_progress(device: str = ""):
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    with _progress_lock:
        prog = g_generation_progress.get(device)
    if prog is None:
        return {"current": 0, "total": 0, "message": "Not started", "done": False}
    return prog


@app.get("/defaults")
async def defaults(device: str = ""):
    # Prefer device-specific defaults, fall back to global
    if device:
        user_defaults = USERS_DIR / device / "defaults.json"
        if user_defaults.exists():
            return json.loads(user_defaults.read_text())
    if not DEFAULTS_PATH.exists():
        return {}
    return json.loads(DEFAULTS_PATH.read_text())


@app.post("/generate")
async def generate(request: Request):
    try:
        payload = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")

    if not isinstance(payload, dict):
        raise HTTPException(status_code=422, detail="JSON body must be an object")

    device = payload.get("device", "").strip()
    name   = payload.get("name",   "").strip()
    desp   = payload.get("desp",   "").strip()

    if not device or not desp:
        raise HTTPException(status_code=400, detail="Missing device or description")

    user_dir = USERS_DIR / device
    user_dir.mkdir(parents=True, exist_ok=True)
    config = _device_config(user_dir)
    config.update({"name": name, "desp": desp})
    (user_dir / "config.json").write_text(json.dumps(config))

    # Always generate a fresh base avatar via Gemini
    await asyncio.to_thread(create_base_avatar, user_dir, desp)
    _mark_files_updated(user_dir, ["config.json", "bin-base.bin"])

    return {"ok": True}


@app.get("/config")
async def get_config(device: str = ""):
    if not device:
        return {}
    config_file = USERS_DIR / device / "config.json"
    if not config_file.exists():
        return {}
    return json.loads(config_file.read_text())


@app.get("/pending_avatar")
async def pending_avatar(device: str = ""):
    if device:
        user_file = USERS_DIR / device / "bin" / "base.bin"
        if user_file.exists():
            return Response(content=user_file.read_bytes(), media_type="application/octet-stream")
    raise HTTPException(status_code=404, detail="No avatar generated yet")


@app.get("/files_to_download")
async def files_to_download(device: str = ""):
    """Return a newline-delimited list of filenames marked as updated (true) in updated.json."""
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    user_dir = USERS_DIR / device
    if not user_dir.exists():
        raise HTTPException(status_code=404, detail="Device not found")
    path = _updated_path(user_dir)
    with _updated_lock:
        data = json.loads(path.read_text()) if path.exists() else {}
    files = [name for name, updated in data.items()
             if updated and not name.startswith(DEVICE_SKIP_PREFIXES)]
    return Response(content="\n".join(files), media_type="text/plain")


@app.post("/mark_downloaded")
async def mark_downloaded(request: Request):
    """ESP32 calls this after successfully downloading files; marks them false in updated.json."""
    try:
        payload = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")
    device = payload.get("device", "").strip()
    files  = payload.get("files", [])
    if not device or not isinstance(files, list):
        raise HTTPException(status_code=400, detail="Missing device or files")
    _mark_files_downloaded(USERS_DIR / device, files)
    return {"ok": True}


@app.get("/list_files")
async def list_files(device: str = ""):
    """Return a newline-delimited list of flat filenames for the device (excluding png folder)."""
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    user_dir = USERS_DIR / device
    if not user_dir.exists():
        raise HTTPException(status_code=404, detail="Device not found")
    files = []
    for path in sorted(user_dir.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(user_dir)
        parts = rel.parts
        flat = "-".join(parts)
        if parts[0] in DEVICE_SKIP_DIRS or flat == "updated.json":
            continue
        files.append(flat)
    return Response(content="\n".join(files), media_type="text/plain")


@app.get("/download_file")
async def download_file(device: str = "", name: str = ""):
    """Serve a single file by its flat name (e.g. bin-angry-0.bin → bin/angry/0.bin)."""
    if not device or not name:
        raise HTTPException(status_code=400, detail="Missing device or name")
    user_dir = USERS_DIR / device
    if not user_dir.exists():
        raise HTTPException(status_code=404, detail="Device not found")
    parts = name.split("-")
    for p in parts:
        if not p or ".." in p or "/" in p or "\\" in p:
            raise HTTPException(status_code=400, detail="Invalid filename")
    file_path = user_dir.joinpath(*parts)
    try:
        file_path.relative_to(user_dir)
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid path")
    if not file_path.exists() or not file_path.is_file():
        raise HTTPException(status_code=404, detail="File not found")
    return Response(content=file_path.read_bytes(), media_type="application/octet-stream")


def _build_sensor_list(payload: dict) -> list[str]:
    """Collect enabled sensor labels from a save_sensors payload."""
    sensors = []
    for key in ("temperature", "humidity", "sound"):
        if payload.get(key):
            sensors.append(SENSOR_LABELS[key])
    for pin in ("pin1", "pin2", "pin3", "pin4"):
        val = payload.get(pin, "")
        if val and val in SENSOR_LABELS:
            sensors.append(SENSOR_LABELS[val])
    print(sensors)
    return sensors


def _generate_defaults(object_name: str, sensors: list[str]) -> dict:
    """Blocking call to Gemini; run via asyncio.to_thread."""
    positive = get_positive_attributes(object_name, sensors)
    negative = get_negative_attributes(object_name, sensors)
    return {"positive": positive, "negative": negative}


@app.post("/save_sensors")
async def save_sensors(request: Request):
    try:
        payload = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")
    device = payload.get("device", "").strip()
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    user_dir = USERS_DIR / device
    user_dir.mkdir(parents=True, exist_ok=True)
    (user_dir / "sensors.json").write_text(json.dumps(payload))
    _mark_files_updated(user_dir, ["sensors.json"])

    # Generate per-user defaults.json only if it doesn't exist yet
    if not (user_dir / "defaults.json").exists():
        config_file = user_dir / "config.json"
        if config_file.exists():
            config = json.loads(config_file.read_text())
            object_name = config.get("desp") or config.get("name", "")
            sensors = _build_sensor_list(payload)
            if object_name and sensors:
                print(f"Generating defaults for '{object_name}' with sensors {sensors}…")
                defaults = await asyncio.to_thread(_generate_defaults, object_name, sensors)
                (user_dir / "defaults.json").write_text(json.dumps(defaults))
                _mark_files_updated(user_dir, ["defaults.json"])
                print("defaults.json written.")

    return {"ok": True}


@app.post("/regenerate_defaults")
async def regenerate_defaults(request: Request):
    try:
        payload = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")
    device = payload.get("device", "").strip()
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    user_dir = USERS_DIR / device
    config_file  = user_dir / "config.json"
    sensors_file = user_dir / "sensors.json"
    if not config_file.exists() or not sensors_file.exists():
        raise HTTPException(status_code=404, detail="Device config or sensors not found")
    config      = json.loads(config_file.read_text())
    sensors_cfg = json.loads(sensors_file.read_text())
    object_name = config.get("desp") or config.get("name", "")
    sensors     = _build_sensor_list(sensors_cfg)
    if not object_name or not sensors:
        raise HTTPException(status_code=422, detail="Not enough info to generate defaults")
    print(f"Regenerating defaults for '{object_name}' with sensors {sensors}…")
    defaults = await asyncio.to_thread(_generate_defaults, object_name, sensors)
    (user_dir / "defaults.json").write_text(json.dumps(defaults))
    _mark_files_updated(user_dir, ["defaults.json"])
    print("defaults.json regenerated.")
    return {"ok": True, "positive": defaults["positive"], "negative": defaults["negative"]}


@app.get("/sensors_config")
async def sensors_config(device: str = ""):
    if not device:
        return {}
    config_file = USERS_DIR / device / "sensors.json"
    if not config_file.exists():
        return {}
    return json.loads(config_file.read_text())


@app.post("/save_behaviors")
async def save_behaviors(request: Request):
    try:
        payload = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")
    device = payload.get("device", "").strip()
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    user_dir = USERS_DIR / device
    user_dir.mkdir(parents=True, exist_ok=True)

    # Compare with existing behaviors — only log and update if actually changed
    behaviors_path = user_dir / "behaviors.json"
    old_positive = ""
    old_negative = ""
    if behaviors_path.exists():
        try:
            old = json.loads(behaviors_path.read_text())
            old_positive = old.get("positive", "")
            old_negative = old.get("negative", "")
        except Exception:
            pass

    new_positive = payload.get("positive", "")
    new_negative = payload.get("negative", "")

    if new_positive != old_positive or new_negative != old_negative:
        # Log the new behaviors to logs/
        logs_dir = user_dir / "logs"
        logs_dir.mkdir(exist_ok=True)
        ts = _device_now(user_dir).strftime("%Y-%m-%d_%H-%M-%S")
        log_entry = {"positive": new_positive, "negative": new_negative}
        (logs_dir / f"behavior_{ts}.json").write_text(json.dumps(log_entry, indent=2))

        (user_dir / "behaviors.json").write_text(json.dumps(payload))
        _mark_files_updated(user_dir, ["behaviors.json"])

    # # Generate emotion_logic.txt (Method 1 on-device evaluation)
    # config_file = user_dir / "config.json"
    # if config_file.exists():
    #     config      = json.loads(config_file.read_text())
    #     object_name = config.get("desp") or config.get("name", "")
    #     positive    = payload.get("positive", "")
    #     negative    = payload.get("negative", "")
    #     if object_name and (positive or negative):
    #         print(f"Generating emotion_logic.txt for '{object_name}'…")
    #         logic_text = await asyncio.to_thread(
    #             generate_emotion_logic, object_name, positive, negative
    #         )
    #         (user_dir / "emotion_logic.txt").write_text(logic_text)
    #         _mark_files_updated(user_dir, ["emotion_logic.txt"])
    #         print("emotion_logic.txt written.")

    return {"ok": True}

'''
@app.get("/get_emotion")
async def get_emotion(device: str = "", voltage: float = 0.0, raw: int = 0):
    """Method 2: device sends live sensor values; server uses Gemini to pick emotion."""
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    user_dir      = USERS_DIR / device
    behaviors_file = user_dir / "behaviors.json"
    if not behaviors_file.exists():
        return {"emotion": "calm"}
    behaviors = json.loads(behaviors_file.read_text())
    positive  = behaviors.get("positive", "")
    negative  = behaviors.get("negative", "")
    sensor_values = {"voltage": round(voltage, 2), "raw": raw}
    emotion = await asyncio.to_thread(determine_emotion, positive, negative, sensor_values)
    return {"emotion": emotion}
'''

def _save_inference_output(user_dir: Path, emotion: str, reason: str, sensor_history: dict):
    """Save sensor history and inference result to a datetime-named file under arxiv/."""
    arxiv_dir = user_dir / "logs"
    arxiv_dir.mkdir(parents=True, exist_ok=True)
    filename = _device_now(user_dir).strftime("%Y-%m-%d_%H-%M-%S") + ".json"
    data = {
        "emotion": emotion,
        "reason": reason,
        "sensor_history": sensor_history,
    }
    (arxiv_dir / filename).write_text(json.dumps(data, indent=2))


@app.get("/get_emotion")
async def get_emotion(device: str = ""):
    """Determine emotion using stored sensor history, config, and behaviors for a device."""
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    user_dir       = USERS_DIR / device
    config_file    = user_dir / "config.json"
    behaviors_file = user_dir / "behaviors.json"
    hist_file      = user_dir / "sensor_history.json"
    if not behaviors_file.exists() or not hist_file.exists():
        raise HTTPException(status_code=404, detail="Missing behaviors or sensor history")
    config    = json.loads(config_file.read_text()) if config_file.exists() else {}
    behaviors = json.loads(behaviors_file.read_text())
    hist      = json.loads(hist_file.read_text())
    start_time = time.time()
    emotion, reason = await asyncio.to_thread(
        determine_emotion,
        object_desp     = config.get("desp", ""),
        positive        = behaviors.get("positive", ""),
        negative        = behaviors.get("negative", ""),
        sensor_values   = hist.get("sensors", {}),
        current_datetime = hist.get("current_datetime", ""),
    )
    inference_time = round(time.time() - start_time, 3)
    print(f"[get_emotion] device={device}  emotion={emotion}  reason={reason}  inference_time={inference_time}s")
    _save_inference_output(user_dir, emotion, reason, hist)
    return {"emotion": emotion, "reason": reason}


def _set_progress(device: str, current: int, total: int, message: str, done: bool = False):
    """Thread-safe progress update — called from both async context and worker threads."""
    with _progress_lock:
        prev = g_generation_progress.get(device, {})
        g_generation_progress[device] = {
            "current": current, "total": total, "message": message, "done": done,
            "latest_bin": prev.get("latest_bin", ""),
        }


async def _run_emotions_with_progress(device: str, user_dir: Path):
    def callback(current: int, total: int, message: str):
        _set_progress(device, current, total, message)

    def on_file_saved(flat_name: str):
        _mark_files_updated(user_dir, [flat_name])
        if flat_name.startswith("bin-"):
            with _progress_lock:
                if device in g_generation_progress:
                    g_generation_progress[device]["latest_bin"] = flat_name

    _set_progress(device, 0, 0, "Starting…")
    await asyncio.to_thread(create_all_emotions, user_dir, callback, on_file_saved)
    with _progress_lock:
        total = g_generation_progress[device].get("total", 0)
    _set_progress(device, total, total, "All done!", done=True)


@app.post("/start_generation")
async def start_generation(request: Request):
    """Called when the user approves their avatar ('I like it!').
    If bin-base.bin is marked updated, starts emotion generation in the background."""
    try:
        payload = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")
    device = payload.get("device", "").strip()
    if not device:
        raise HTTPException(status_code=400, detail="Missing device")
    user_dir = USERS_DIR / device
    if not user_dir.exists():
        raise HTTPException(status_code=404, detail="Device not found")

    # Update config.json if name/desp provided
    name = payload.get("name", "").strip()
    desp = payload.get("desp", "").strip()
    if name or desp:
        config_path = user_dir / "config.json"
        config = json.loads(config_path.read_text()) if config_path.exists() else {}
        if name:
            config["name"] = name
        if desp:
            config["desp"] = desp
        config_path.write_text(json.dumps(config))
        _mark_files_updated(user_dir, ["config.json"])

    if payload.get("generated"):
        for path in (user_dir / "png").rglob("*") if (user_dir / "png").exists() else []:
            if path.is_file() and path.name != "base.png":
                path.unlink()
        for path in (user_dir / "bin").rglob("*") if (user_dir / "bin").exists() else []:
            if path.is_file() and path.name != "base.bin":
                path.unlink()

    asyncio.create_task(_run_emotions_with_progress(device, user_dir))
    return {"ok": True, "started": True}


@app.get("/behaviors_config")
async def behaviors_config(device: str = ""):
    if not device:
        return {}
    config_file = USERS_DIR / device / "behaviors.json"
    if not config_file.exists():
        return {}
    return json.loads(config_file.read_text())


@app.post("/register")
async def register_device(request: Request):
    """Called by the ESP32 after WiFi provisioning to create the device's user folder."""
    try:
        payload = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")
    device = payload.get("device", "").strip().upper()
    if not re.fullmatch(r"[0-9A-Za-z]{6}|_[0-9A-Za-z]{5}", device):
        raise HTTPException(status_code=400, detail="Invalid device ID")
    (USERS_DIR / device).mkdir(parents=True, exist_ok=True)
    await _ensure_timezone(USERS_DIR / device, request)
    return {"ok": True}


@app.post("/sensor_history")
async def sensor_history(request: Request):
    """Receive and store per-sensor history from a device.

    Expected body:
    {
      "device": "<id>",
      "sensors": {
        "<sensor name>": {
          "current value": <number>,
          "history": [<number>, ...]   // up to 336 hourly samples
        },
        ...
      },
      "battery": {"mv": <int>, "percent": <int>, "usb": <bool>}   // optional
    }
    "battery" is kept beside "sensors" (not inside) so it is stored and shown
    but never fed to the emotion inference as an environment sensor.
    """
    try:
        payload = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")
    device  = payload.get("device", "unknown")
    sensors = payload.get("sensors", {})
    battery = payload.get("battery")
    print(f"[sensor_history] device={device}  sensors={list(sensors.keys())}  battery={battery}")

    for name, data in sensors.items():
        print(f"  {name}: current={data.get('current value')}  history_len={len(data.get('history', []))}")

    # Persist to per-device sensor_history.json (overwrite with latest snapshot)
    user_dir = USERS_DIR / device
    if user_dir.exists():
        await _ensure_timezone(user_dir, request)
        hist_path = user_dir / "sensor_history.json"
        local_time = _device_now(user_dir).strftime("%Y-%m-%d %H:%M:%S")
        snapshot = {
            "device": device,
            "current_datetime": local_time,
            "timezone": _device_tz(user_dir).key,
            "sensors": sensors,
        }
        if isinstance(battery, dict):
            snapshot["battery"] = battery
        hist_path.write_text(json.dumps(snapshot, indent=2))

    return {"ok": True}


@app.get("/all-oat-devices")
async def all_oat_devices_page():
    """Serve the devices list page."""
    return FileResponse("static/all-oat-devices.html")


@app.get("/all-oat-devices-list")
async def all_oat_devices_list():
    """Return list of registered device IDs with last seen timestamp."""
    if not USERS_DIR.exists():
        return []
    devices = []
    for d in sorted(USERS_DIR.iterdir(), key=lambda p: p.name):
        if not d.is_dir():
            continue
        info = {"id": d.name, "current_datetime": None, "name": None, "desp": None}
        sensor_file = d / "sensor_history.json"
        if sensor_file.exists():
            try:
                data = json.loads(sensor_file.read_text())
                info["current_datetime"] = data.get("current_datetime")
            except Exception:
                pass
        config_file = d / "config.json"
        if config_file.exists():
            try:
                cfg = json.loads(config_file.read_text())
                info["name"] = cfg.get("name")
                info["desp"] = cfg.get("desp")
            except Exception:
                pass
        devices.append(info)
    return devices


@app.get("/gallery")
async def gallery_page():
    """Serve the gallery page showing all devices with their neutral avatars."""
    return FileResponse("static/gallery.html")


@app.get("/{short_id}")
async def device_shortlink(short_id: str):
    """Redirect oats.daehwakim.com/<last6> to the avatar setup page."""
    if not re.fullmatch(r"[0-9A-Za-z]{6}|_[0-9A-Za-z]{5}", short_id):
        raise HTTPException(status_code=404, detail="Not found")
    if not (USERS_DIR / short_id.upper()).exists():
        return HTMLResponse(
            status_code=404,
            content=f"<h2>Device not found</h2><p>Device <b>{short_id.upper()}</b> is not registered."
                    " Please double click the gear button on the OAT device and complete WiFi setup first.</p>",
        )
    return RedirectResponse(url=f"/main?device={short_id.upper()}")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=4070)
