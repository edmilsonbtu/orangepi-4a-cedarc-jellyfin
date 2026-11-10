#!/usr/bin/env python3
import os
import re
import sys
import json
import subprocess
from pathlib import Path

REAL = "/usr/lib/jellyfin-ffmpeg/ffmpeg"
CONTROL = "/opt/cedar-native/jellyfin-test/cedar-process-control.py"

args = sys.argv[1:]

def opt(name, default=None):
    try:
        return args[args.index(name) + 1]
    except (ValueError, IndexError):
        return default

def fallback():
    os.execv(REAL, [REAL, *args])

source = opt("-i")
vf = opt("-vf", "")
output = args[-1] if args else None
pattern = opt("-hls_segment_filename")
encoder = opt("-codec:v:0")
segment_type = opt("-hls_segment_type")
seek = opt("-ss")
start = opt("-start_number", "0")

sub = re.search(r"subtitles=f='([^']+)'", vf)
fonts = re.search(r"fontsdir='([^']+)'", vf)

if source and source.startswith("file:"):
    source = source[5:]

compatible = all([
    source,
    output,
    pattern,
    sub,
    encoder == "libx264",
    segment_type == "mpegts",
    str(source).lower().endswith(".mkv"),
    seek is None or bool(re.fullmatch(r"[0-9:.]+", seek)),
    start.isdigit(),
    os.environ.get("CEDAR_JELLYFIN_ENABLE") == "1",
])

if not compatible:
    fallback()

subtitle = sub.group(1)
fontdir = fonts.group(1) if fonts else ""

if not all(map(os.path.isfile, [source, subtitle])):
    fallback()

env = os.environ.copy()
env.update({
    "CEDAR_SUBTITLE_FILE": subtitle,
    "CEDAR_SUBTITLE_FONTS": fontdir,
    "CEDAR_SEEK_SECONDS": str(
        sum(float(v) * 60 ** i
            for i, v in enumerate(reversed((seek or "0").split(":"))))
    ),
    "CEDAR_MAX_PACKETS": "0",
    "CEDAR_TIMEOUT": "180",
    "CEDAR_STREAM_TS": "1",
    "CEDAR_SESSION_ID": "cedar-" + os.urandom(16).hex(),
})

logdir = Path("/var/cache/jellyfin/cedar-wrapper")
logdir.mkdir(parents=True, exist_ok=True)
env["CEDAR_LOG"] = str(logdir / ("session-" + env["CEDAR_SESSION_ID"] + ".log"))

cmd = [
    sys.executable, CONTROL, source,
    "-hide_banner", "-nostdin", "-loglevel", "warning",
    "-f", "mpegts", "-i", "pipe:0",
    *(
        ["-ss", str(sum(
            float(v) * 60 ** i
            for i, v in enumerate(reversed(seek.split(":")))
        ))]
        if seek else []
    ),
    "-i", source,
    "-map", "0:v:0", "-map", "1:a:0",
    "-c:v", "copy", "-c:a", "aac",
    "-b:a", "192k", "-ac", "2",
    "-f", "hls", "-hls_time", "3",
    "-hls_segment_type", "mpegts",
    "-hls_playlist_type", "vod",
    "-hls_list_size", "0",
    "-start_number", start,
    "-hls_segment_filename", pattern,
    "-y", output,
]

with (logdir / "adapter.jsonl").open("a") as f:
    f.write(json.dumps({
        "mode": "cedar",
        "source": source,
        "subtitle": subtitle,
        "output": output,
    }) + "\n")

os.execve(sys.executable, cmd, env)
