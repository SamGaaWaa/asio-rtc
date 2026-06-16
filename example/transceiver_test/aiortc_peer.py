#!/usr/bin/env python3
"""aiortc peer for transceiver test — receives C++ offer, sends answer."""

import asyncio
import json
import sys
import time
import fractions
import websockets
from aiortc import (
    RTCPeerConnection,
    RTCSessionDescription,
    VideoStreamTrack,
    AudioStreamTrack,
)
from av import AudioFrame, VideoFrame


class TestVideoTrack(VideoStreamTrack):
    kind = "video"

    def __init__(self):
        super().__init__()
        self._timestamp = 0
        self._start_time = None

    async def recv(self):
        if self._start_time is None:
            self._start_time = time.time()
        pts = self._timestamp
        self._timestamp += 3000
        elapsed = time.time() - self._start_time
        target = pts / 90000.0
        sleep_t = target - elapsed
        if sleep_t > 0:
            await asyncio.sleep(sleep_t)
        frame = VideoFrame(width=640, height=480, format="yuv420p")
        y_size = 640 * 480
        uv_size = y_size // 4
        frame.planes[0].update(b"\x80" * y_size)
        frame.planes[1].update(b"\x80" * uv_size)
        frame.planes[2].update(b"\x80" * uv_size)
        frame.pts = pts
        frame.time_base = fractions.Fraction(1, 90000)
        return frame


class TestAudioTrack(AudioStreamTrack):
    kind = "audio"

    def __init__(self):
        super().__init__()
        self._timestamp = 0
        self._start_time = None

    async def recv(self):
        if self._start_time is None:
            self._start_time = time.time()
        pts = self._timestamp
        samples = 960  # 20ms at 48kHz
        self._timestamp += samples
        elapsed = time.time() - self._start_time
        target = pts / 48000.0
        sleep_t = target - elapsed
        if sleep_t > 0:
            await asyncio.sleep(sleep_t)
        frame = AudioFrame(format="s16", layout="mono", samples=samples)
        frame.sample_rate = 48000
        frame.pts = pts
        frame.time_base = fractions.Fraction(1, 48000)
        return frame


async def main(server="ws://localhost:8085/ws"):
    print(f"Connecting to {server}...")
    ws = await websockets.connect(server)
    print("Connected to signaling server")

    pc = RTCPeerConnection()
    pc_complete = asyncio.Event()
    recv_video_count = [0]
    recv_audio_count = [0]

    video_track = TestVideoTrack()
    audio_track = TestAudioTrack()
    pc.addTrack(video_track)
    pc.addTrack(audio_track)
    print("Added local video track (640x480, 30fps) + audio track (48kHz)")

    @pc.on("track")
    def on_track(track):
        print(f"Remote track received: {track.kind}")
        if track.kind == "video":
            asyncio.ensure_future(consume_remote(track, recv_video_count, "video"))
        elif track.kind == "audio":
            asyncio.ensure_future(consume_remote(track, recv_audio_count, "audio"))

    @pc.on("connectionstatechange")
    async def on_connection_state():
        state = pc.connectionState
        print(f"Peer connection state: {state}")
        if state in ("connected", "failed"):
            pc_complete.set()

    msg = await ws.recv()
    msg = json.loads(msg)
    print(f"Got offer (type={msg['type']})")
    offer_sdp = msg["sdp"]
    print(f"  SDP: {offer_sdp[:200]}...")

    await pc.setRemoteDescription(
        RTCSessionDescription(sdp=offer_sdp, type="offer")
    )

    answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)
    print(f"Sending answer")
    await ws.send(
        json.dumps({"type": "answer", "sdp": pc.localDescription.sdp})
    )

    print("Waiting for peer connection...")
    while not pc_complete.is_set():
        await asyncio.sleep(1)
    print(f"Final state: ICE={pc.iceConnectionState} DTLS={pc._RTCPeerConnection__dtlsTransports[0].state if hasattr(pc, '_RTCPeerConnection__dtlsTransports') and pc._RTCPeerConnection__dtlsTransports else '?'}")

    print("Running for 10 seconds...")
    await asyncio.sleep(10)

    print(f"Remote video frames received: {recv_video_count[0]}")
    print(f"Remote audio frames received: {recv_audio_count[0]}")
    await pc.close()
    await ws.close()
    print("Done")


async def consume_remote(track, counter, label):
    print(f"Receiving remote {label} track")
    while True:
        try:
            frame = await track.recv()
            counter[0] += 1
            if counter[0] == 1:
                if label == "video":
                    print(f"First remote {label} frame: {frame.width}x{frame.height}")
                else:
                    print(f"First remote {label} frame: {frame.samples}samples@{frame.sample_rate}Hz")
            elif counter[0] % 10 == 0:
                print(f"Remote {label} frames: {counter[0]}")
        except Exception as e:
            print(f"Remote {label} track ended: {e}")
            break


if __name__ == "__main__":
    asyncio.run(main())
