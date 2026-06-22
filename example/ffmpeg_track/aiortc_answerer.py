#!/usr/bin/env python3
"""Answerer for ffmpeg_track example — receives C++ offer, returns answer."""

import asyncio
import json
import sys
import websockets
from aiortc import RTCPeerConnection, RTCSessionDescription


async def main(server="ws://localhost:8086/ws"):
    ws = await websockets.connect(server)
    print("Connected to signaling server")

    pc = RTCPeerConnection()
    pc_complete = asyncio.Event()
    frame_count = [0]

    @pc.on("track")
    def on_track(track):
        print(f"Remote track received: {track.kind}")
        asyncio.ensure_future(consume_remote(track, frame_count))

    @pc.on("connectionstatechange")
    async def on_connection_state():
        state = pc.connectionState
        print(f"Peer connection state: {state}")
        if state in ("connected", "failed"):
            pc_complete.set()

    # Receive offer
    msg = await ws.recv()
    msg = json.loads(msg)
    print(f"Got {msg['type']}, SDP={msg['sdp'][:100]}...")

    await pc.setRemoteDescription(
        RTCSessionDescription(sdp=msg["sdp"], type="offer"))

    # Create and send answer
    answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)
    await ws.send(json.dumps(
        {"type": "answer", "sdp": pc.localDescription.sdp}))
    print("Sent answer")

    print("Waiting for connection...")
    while not pc_complete.is_set():
        await asyncio.sleep(1)
    print(f"Final state: {pc.connectionState}")

    print("Receiving video for 15 seconds...")
    await asyncio.sleep(15)

    print(f"Total frames received: {frame_count[0]}")
    await pc.close()
    await ws.close()


async def consume_remote(track, counter):
    print(f"Receiving {track.kind} track")
    while True:
        try:
            frame = await track.recv()
            counter[0] += 1
            if counter[0] == 1:
                print(f"  first frame received")
            elif counter[0] % 30 == 0:
                print(f"  {counter[0]} frames received")
        except Exception as e:
            print(f"  track ended: {e}")
            break


if __name__ == "__main__":
    asyncio.run(main())
