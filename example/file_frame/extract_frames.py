#!/usr/bin/env python3
"""Extract H.264 access units and Opus packets for the file_frame example."""

import subprocess
import sys
import shutil
from pathlib import Path

INPUT = sys.argv[1] if len(sys.argv) > 1 else str(
    Path(__file__).resolve().parent.parent.parent / "test.webm")
ROOT = Path(__file__).resolve().parent
VIDEO_DIR = ROOT / "video_frame"
AUDIO_DIR = ROOT / "audio_frame"

for d in [VIDEO_DIR, AUDIO_DIR]:
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)

# --- Video ---
print("Transcoding to H.264 and extracting frames...")
proc = subprocess.Popen(
    ["ffmpeg", "-i", INPUT, "-an",
     "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
     "-g", "60", "-x264-params", "sliced-threads=0",
     "-f", "h264", "pipe:1"],
    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
data = proc.stdout.read()
proc.wait()

if not data:
    print("ERROR: ffmpeg produced no output")
    sys.exit(1)

nal_offsets = []  # (position, start_code_len)
i = 0
while i + 2 < len(data):
    if data[i] == 0 and data[i + 1] == 0:
        if data[i + 2] == 1:
            nal_offsets.append((i, 3))
            i += 3
        elif i + 3 < len(data) and data[i + 2] == 0 and data[i + 3] == 1:
            nal_offsets.append((i, 4))
            i += 4
        else:
            i += 1
    else:
        i += 1

if not nal_offsets:
    print("ERROR: no NAL units found")
    sys.exit(1)

sps, pps = b"", b""
frames = []
current = b""
has_vcl = False

for idx, (pos, sc_len) in enumerate(nal_offsets):
    nal_data_start = pos + sc_len
    nal_end = (nal_offsets[idx + 1][0]
               if idx + 1 < len(nal_offsets) else len(data))
    nal_type = data[nal_data_start] & 0x1F

    if nal_type == 7:
        sps = data[pos:nal_end]
    elif nal_type == 8:
        pps = data[pos:nal_end]
    elif nal_type == 5:  # IDR
        if has_vcl:
            frames.append(sps + pps + current)
            current = b""
        current += sps + pps + data[pos:nal_end]
        has_vcl = True
    elif nal_type == 1:  # non-IDR
        if has_vcl:
            frames.append(current)
            current = b""
        current += data[pos:nal_end]
        has_vcl = True
    else:
        current += data[pos:nal_end]

if current:
    frames.append(current)

for idx, f in enumerate(frames):
    (VIDEO_DIR / f"{idx + 1:04d}.h264").write_bytes(f)
print(f"  {len(frames)} video frames")

# --- Audio ---
print("Extracting Opus frames...")
subprocess.run(
    ["ffmpeg", "-i", INPUT, "-vn",
     "-c:a", "libopus", "-b:a", "64k", "-ar", "48000", "-ac", "2",
     "-f", "segment", "-segment_time", "0.02", "-reset_timestamps", "1",
     str(AUDIO_DIR / "tmp_%04d.opus")],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)

tmp_files = sorted(AUDIO_DIR.glob("tmp_*.opus"))
for i, f in enumerate(tmp_files):
    f.rename(AUDIO_DIR / f"{i + 1:04d}.opus")
print(f"  {len(tmp_files)} audio frames")
print("Done!")
