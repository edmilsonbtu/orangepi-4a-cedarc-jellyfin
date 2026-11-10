#!/opt/cedar-native/venv/bin/python
import re
import math
import av
from fractions import Fraction
import sys
import os
import struct
import subprocess
import threading
import time
import signal
import uuid
import shlex
from pathlib import Path

SOURCE = sys.argv[1] if len(sys.argv) > 1 else "/home/user/videos-teste/teste-h264.mp4"
OUTPUT = os.environ.get("CEDAR_OUTPUT", "/home/user/videos-teste/cedar-auto-v7.12.1-test.h264")
MP4_OUTPUT = os.environ.get("CEDAR_MP4_OUTPUT", "/home/user/videos-teste/cedar-auto-v7.12.1-test.mp4")
LOG = os.environ.get("CEDAR_LOG", "/tmp/cedar-auto-v7.12.1.log")
MAX_PACKETS = int(os.environ.get("CEDAR_MAX_PACKETS", "0"))
CEDAR_TIMEOUT = int(os.environ.get("CEDAR_TIMEOUT", "600"))
STREAM_STDOUT = os.environ.get("CEDAR_STREAM_STDOUT", "0") == "1"
STREAM_TS = os.environ.get("CEDAR_STREAM_TS", "0") == "1"

SEEK_SECONDS = float(
    os.environ.get("CEDAR_SEEK_SECONDS", "0")
)

if not math.isfinite(SEEK_SECONDS) or SEEK_SECONDS < 0:
    raise SystemExit("CEDAR_SEEK_SECONDS_INVALID")

SESSION_ID = os.environ.get("CEDAR_SESSION_ID") or (
    "cedar-" + uuid.uuid4().hex
)
if not re.fullmatch(r"cedar-[0-9a-f]{32}", SESSION_ID):
    raise SystemExit("CEDAR_SESSION_ID inválido")
REMOTE_PIDFILE = f"/tmp/cedar-session-{SESSION_ID}.pid"
ACTIVE_PROC = None






def cleanup_remote():
    """Encerra somente o grupo do cedar-mux criado por este feeder."""
    proc = ACTIVE_PROC
    if proc is None or proc.poll() is not None:
        return True

    try:
        # start_new_session=True faz o PID ser líder da sessão.
        if os.getpgid(proc.pid) != proc.pid:
            return False

        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
            proc.wait(timeout=5)
        return True
    except ProcessLookupError:
        return True
    except (OSError, subprocess.TimeoutExpired):
        return False


def handle_signal(signum, frame):
    global CANCEL_SIGNAL
    CANCEL_SIGNAL = signum
    raise SystemExit(128 + signum)


CANCEL_SIGNAL = None
signal.signal(signal.SIGTERM, handle_signal)
signal.signal(signal.SIGINT, handle_signal)

def read_exact(stream, size):
    result = bytearray()
    while len(result) < size:
        part = stream.read(size - len(result))
        if not part:
            if not result:
                return None
            raise EOFError("Saida CedarC truncada")
        result.extend(part)
    return bytes(result)

def receive_ts(proc, state):
    first_pts = None
    previous_pts = None

    try:
        with av.open(sys.__stdout__.buffer, mode="w", format="mpegts",
                     options={"mpegts_flags": "resend_headers"}) as mux:

            stream = mux.add_stream("h264", rate=state["rate"])
            stream.width = state["width"]
            stream.height = state["height"]
            stream.sample_aspect_ratio = state["sar"]
            stream.codec_context.sample_aspect_ratio = state["sar"]
            stream.time_base = Fraction(1, 90000)
            stream.codec_context.time_base = Fraction(1, 90000)

            while True:
                header = read_exact(proc.stdout, 12)
                if header is None:
                    break

                size, pts = struct.unpack("<Iq", header)

                if size == 0 or size > 4 * 1024 * 1024:
                    raise ValueError(f"Tamanho invalido: {size}")

                payload = read_exact(proc.stdout, size)
                if payload is None:
                    raise EOFError("Payload ausente")

                if first_pts is None:
                    first_pts = pts

                normalized_pts = pts - first_pts

                if previous_pts is not None and normalized_pts <= previous_pts:
                    raise ValueError("PTS nao monotono")

                previous_pts = normalized_pts

                packet = av.Packet(payload)
                packet.pts = normalized_pts
                packet.dts = normalized_pts
                packet.time_base = Fraction(1, 1000000)
                packet.stream = stream

                mux.mux(packet)

                state["frames"] += 1
                state["last_pts"] = pts

    except Exception as exc:
        state["error"] = str(exc)


def receive_stream(proc, state):
    first_pts = None
    previous_pts = None

    try:
        while True:
            header = read_exact(proc.stdout, 12)
            if header is None:
                break

            size, pts = struct.unpack("<Iq", header)

            if size == 0 or size > 4 * 1024 * 1024:
                raise ValueError(f"Tamanho invalido: {size}")

            payload = read_exact(proc.stdout, size)
            if payload is None:
                raise EOFError("Payload ausente")

            if first_pts is None:
                first_pts = pts

            normalized_pts = pts - first_pts

            if previous_pts is not None and normalized_pts <= previous_pts:
                raise ValueError("PTS nao monotono")

            previous_pts = normalized_pts

            sys.__stdout__.buffer.write(payload)
            sys.__stdout__.buffer.flush()

            state["frames"] += 1
            state["last_pts"] = pts

    except Exception as exc:
        state["error"] = str(exc)


def receive(proc, state):
    try:
        if STREAM_TS:
            receive_ts(proc, state)
            return

        if STREAM_STDOUT:
            receive_stream(proc, state)
            return

        with open(OUTPUT, "wb") as raw, av.open(MP4_OUTPUT, "w") as mux:
            stream = mux.add_stream("h264", rate=state["rate"])
            stream.width = state["width"]
            stream.height = state["height"]
            stream.sample_aspect_ratio = state["sar"]
            stream.codec_context.sample_aspect_ratio = state["sar"]
            stream.time_base = Fraction(1, 1000000)
            stream.codec_context.time_base = Fraction(1, 1000000)

            first_pts = None
            previous_pts = None

            while True:
                header = read_exact(proc.stdout, 12)
                if header is None:
                    break

                size, pts = struct.unpack("<Iq", header)

                if size == 0 or size > 4 * 1024 * 1024:
                    raise ValueError(f"Tamanho invalido: {size}")

                payload = read_exact(proc.stdout, size)
                if payload is None:
                    raise EOFError("Payload ausente")

                raw.write(payload)

                if first_pts is None:
                    first_pts = pts

                normalized_pts = pts - first_pts

                if previous_pts is not None and normalized_pts <= previous_pts:
                    raise ValueError(
                        f"PTS nao monotono: {normalized_pts} <= {previous_pts}"
                    )

                previous_pts = normalized_pts

                packet = av.Packet(payload)
                packet.pts = normalized_pts
                packet.dts = normalized_pts
                packet.time_base = Fraction(1, 1000000)
                packet.stream = stream

                mux.mux(packet)

                state["frames"] += 1
                state["last_pts"] = pts

    except Exception as exc:
        state["error"] = str(exc)

def collect_logs(proc):
    with open(LOG, "wb") as log:
        while True:
            data = proc.stderr.read(65536)
            if not data:
                break
            log.write(data)

if STREAM_STDOUT or STREAM_TS:
    _original_stdout = sys.stdout
    sys.stdout = sys.stderr

def main():
    state = {"frames": 0, "last_pts": None, "error": None, "rate": None}
    start = time.monotonic()
    packets = 0
    error = None

    with av.open(SOURCE) as container:
        video = container.streams.video[0]
        codec = video.codec_context.name

        filters = {
            "h264": "h264_mp4toannexb",
            "hevc": "hevc_mp4toannexb",
        }

        if codec not in filters:
            raise RuntimeError(f"Codec nao suportado: {codec}")

        width = video.codec_context.width
        height = video.codec_context.height

        if (width, height) not in {(640, 480), (1280, 720), (1920, 1080), (854, 480), (1024, 576), (1366, 768), (1408, 768)}:
            raise RuntimeError(
                f"Resolucao ainda nao suportada: {width}x{height}"
            )

        rate = video.average_rate
        if rate is None or rate.numerator <= 0 or rate.denominator <= 0:
            raise RuntimeError(f"FPS invalido: {rate}")

        fps = (rate.numerator * 2 + rate.denominator) // (2 * rate.denominator)

        if not 1 <= fps <= 60:
            raise RuntimeError(f"FPS fora do limite: {fps}")

        state["rate"] = Fraction(rate.numerator, rate.denominator)
        state["width"] = width
        state["height"] = height
        sar = video.sample_aspect_ratio or video.codec_context.sample_aspect_ratio or Fraction(1, 1)
        state["sar"] = Fraction(sar.numerator, sar.denominator)
        print("INPUT_SAR =", state["sar"])
        print("INPUT_CODEC =", codec)
        print("INPUT_RESOLUTION =", f"{width}x{height}")
        print("INPUT_FPS_ORIGINAL =", rate)
        print("INPUT_FPS_NOMINAL =", fps)

        if SEEK_SECONDS > 0:
            target_pts = int(
                SEEK_SECONDS / float(video.time_base)
            )

            container.seek(
                target_pts,
                stream=video,
                backward=True,
                any_frame=False,
            )

            print(
                "CEDAR_SEEK_REQUESTED =",
                SEEK_SECONDS,
            )
            print(
                "CEDAR_SEEK_TARGET_PTS =",
                target_pts,
            )

        bsf = av.bitstream.BitStreamFilterContext(
            filters[codec], video
        )

        env = os.environ.copy()
        env.update({
            "CEDAR_WIDTH": str(width),
            "CEDAR_HEIGHT": str(height),
            "CEDAR_CODEC": codec,
            "CEDAR_FPS": str(fps),
            "CEDAR_SESSION_ID": SESSION_ID,
        })

        command = [
            "/opt/cedar-native/beta54/cedar-mux-ass-v5.4",
            "-", "-",
            str(round(SEEK_SECONDS * 1000000)),
        ]

        global ACTIVE_PROC
        proc = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            bufsize=0,
            start_new_session=True,
        )
        ACTIVE_PROC = proc

        reader = threading.Thread(
            target=receive, args=(proc, state), daemon=True
        )
        logger = threading.Thread(
            target=collect_logs, args=(proc,), daemon=True
        )
        reader.start()
        logger.start()

        try:
            for packet in container.demux(video):
                for filtered in bsf.filter(packet):
                    data = bytes(filtered)
                    if not data:
                        continue

                    pts = filtered.pts
                    if pts is None:
                        pts = packet.pts

                    if pts is None:
                        raise ValueError("Pacote sem PTS")

                    time_base = filtered.time_base or packet.time_base
                    pts_us = int(pts * time_base * 1000000)

                    if packets == 0:
                        print(
                            "CEDAR_FIRST_INPUT_PTS_US =",
                            pts_us,
                            flush=True,
                        )

                    proc.stdin.write(
                        struct.pack("<Iq", len(data), pts_us)
                    )
                    proc.stdin.write(data)
                    packets += 1

                    if MAX_PACKETS > 0 and packets >= MAX_PACKETS:
                        break

                if MAX_PACKETS > 0 and packets >= MAX_PACKETS:
                    break

        except Exception as exc:
            error = str(exc)

        finally:
            try:
                proc.stdin.close()
            except OSError:
                pass

        try:
            proc.wait(timeout=CEDAR_TIMEOUT + 30)
        except subprocess.TimeoutExpired:
            error = "Tempo limite excedido"
            cleanup_remote()
            proc.wait()

        reader.join(timeout=5)
        logger.join(timeout=5)
        cleaned = cleanup_remote()
        ACTIVE_PROC = None
        if not cleaned:
            error = (error or "") + " REMOTE_SESSION_NOT_CLEAN"

    elapsed = time.monotonic() - start

    print("PACKETS_SENT =", packets)
    print("FRAMES_RECEIVED =", state["frames"])
    print("LAST_PTS =", state["last_pts"])
    print("ELAPSED =", round(elapsed, 2))
    print("PROCESS_EXIT =", proc.returncode)
    print("OUTPUT =", OUTPUT)
    print("MP4_OUTPUT =", MP4_OUTPUT)
    print("LOG =", LOG)

    if error:
        print("FEED_ERROR =", error)
    if state["error"]:
        print("RECEIVE_ERROR =", state["error"])

    if proc.returncode or error or state["error"]:
        raise SystemExit(1)

if __name__ == "__main__":
    try:
        main()
    finally:
        if ACTIVE_PROC is not None:
            try:
                cleanup_remote()
            finally:
                if ACTIVE_PROC.poll() is None:
                    ACTIVE_PROC.terminate()
                    try:
                        ACTIVE_PROC.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        ACTIVE_PROC.kill()
                        ACTIVE_PROC.wait()
