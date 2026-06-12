#!/usr/bin/env python3
"""aiortc SRTP peer — sends video over SRTP and counts received frames."""

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
)
from av import VideoFrame


class ColorVideoTrack(VideoStreamTrack):
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
        frame = VideoFrame(width=320, height=240, format="yuv420p")
        y_size = 320 * 240
        uv_size = y_size // 4
        frame.planes[0].update(b"\x80" * y_size)
        frame.planes[1].update(b"\x80" * uv_size)
        frame.planes[2].update(b"\x80" * uv_size)
        frame.pts = pts
        frame.time_base = fractions.Fraction(1, 90000)
        return frame


def dump_srtp_keys(pc):
    """Dump aiortc DTLS-SRTP key material after connection."""
    try:
        from aiortc.rtcdtlstransport import SRTP_PROFILES
        ts_list = list(pc._RTCPeerConnection__dtlsTransports) if hasattr(pc, '_RTCPeerConnection__dtlsTransports') else []
        if not ts_list:
            print("  DTLS transport not found")
            return
        ts = ts_list[0]
        if ts.state != "connected":
            print(f"  DTLS transport state: {ts.state}")
            return
        ssl_obj = ts._ssl
        if ssl_obj is None:
            print("  ssl_obj is None")
            return
        role = ts._role
        prof_bytes = ssl_obj.get_selected_srtp_profile()
        prof_name = prof_bytes.decode() if prof_bytes else "none"

        srtp_prof = None
        for p in SRTP_PROFILES:
            if p.openssl_profile == prof_bytes:
                srtp_prof = p
                break

        if srtp_prof is None:
            print(f"  SRTP profile not found: {prof_name}")
            return

        kl = srtp_prof.key_length
        sl = srtp_prof.salt_length
        view = ssl_obj.export_keying_material(
            b"EXTRACTOR-dtls_srtp", 2 * (kl + sl)
        )

        c_key = view[0:kl]
        s_key = view[kl : 2 * kl]
        c_salt = view[2 * kl : 2 * kl + sl]
        s_salt = view[2 * kl + sl : 2 * kl + 2 * sl]

        print(f"=== SRTP KEY MATERIAL (Python) ===")
        print(f"  role: {role} (Python is DTLS {role})")
        print(f"  profile: {prof_name} key={kl}B salt={sl}B")
        print(f"  client_write_key ({kl}B): {c_key.hex()}")
        print(f"  server_write_key ({kl}B): {s_key.hex()}")
        print(f"  client_write_salt ({sl}B): {c_salt.hex()}")
        print(f"  server_write_salt ({sl}B): {s_salt.hex()}")
        print(
            f"  send_session uses: {'server' if role == 'server' else 'client'}_write_key+salt"
        )
        print(
            f"  recv_session uses: {'client' if role == 'server' else 'server'}_write_key+salt"
        )
    except Exception as e:
        print(f"  key dump error: {e}", file=sys.stderr)


async def main(server="ws://localhost:8084/ws"):
    ws = await websockets.connect(server)
    print("Connected to signaling server")

    pc = RTCPeerConnection()
    pc_complete = asyncio.Event()
    recv_count = [0]

    video_track = ColorVideoTrack()
    pc.addTrack(video_track)
    print("Added local video track (320x240 gray, 30fps)")

    @pc.on("track")
    def on_track(track):
        print(f"Remote track received: {track.kind}")
        asyncio.ensure_future(consume_remote(track, recv_count))

    @pc.on("iceconnectionstatechange")
    async def on_ice_state():
        state = pc.iceConnectionState
        print(f"ICE state: {state}")
        if state in ("connected", "completed", "failed", "disconnected"):
            pc_complete.set()

    offer = await pc.createOffer()
    await pc.setLocalDescription(offer)
    print("Sending SDP offer")
    print(f"  SDP: {pc.localDescription.sdp[:200]}...")
    await ws.send(
        json.dumps({"type": "offer", "sdp": pc.localDescription.sdp})
    )

    msg = await ws.recv()
    msg = json.loads(msg)
    print("Got SDP answer")
    await pc.setRemoteDescription(
        RTCSessionDescription(sdp=msg["sdp"], type="answer")
    )

    print("Waiting for ICE connection...")
    while not pc_complete.is_set():
        await asyncio.sleep(1)
    print(f"Final ICE state: {pc.iceConnectionState}")

    print("Running for 20 seconds...")
    await asyncio.sleep(20)

    print(f"Remote frames received: {recv_count[0]}")
    await pc.close()
    await ws.close()


async def consume_remote(track, counter):
    print(f"Receiving remote track: {track.kind}")
    while True:
        try:
            frame = await track.recv()
            counter[0] += 1
            if counter[0] == 1:
                print(
                    f"First remote frame: {frame.width}x{frame.height}"
                )
            elif counter[0] % 10 == 0:
                print(f"Remote frames received: {counter[0]}")
        except Exception as e:
            print(f"Remote track ended: {e}")
            break


if __name__ == "__main__":
    asyncio.run(main())
